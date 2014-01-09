/*
 * boblight
 * Copyright (C) Bob  2012 
 * 
 * boblight is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * boblight is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "util/log.h"
#include "util/misc.h"
#include "devicespi.h"
#include "util/timeutils.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

CDeviceSPI::CDeviceSPI(CClientsHandler& clients) : m_timer(&m_stop), CDevice(clients)
{
  m_buff = NULL;
  m_buffsize = 0;
  m_fd = -1;
}

void CDeviceSPI::Sync()
{
  if (m_allowsync)
    m_timer.Signal();
}

bool CDeviceSPI::SetupDevice()
{
  m_timer.SetInterval(m_interval);

  m_fd = open(m_output.c_str(), O_RDWR);
  if (m_fd == -1)
  {
    LogError("%s: Unable to open %s: %s", m_name.c_str(), m_output.c_str(), GetErrno().c_str());
    return false;
  }

  int value = SPI_MODE_0;

  if (ioctl(m_fd, SPI_IOC_WR_MODE, &value) == -1)
  {
    LogError("%s: SPI_IOC_WR_MODE: %s", m_name.c_str(), GetErrno().c_str());
    return false;
  }

  if (ioctl(m_fd, SPI_IOC_RD_MODE, &value) == -1)
  {
    LogError("%s: SPI_IOC_RD_MODE: %s", m_name.c_str(), GetErrno().c_str());
    return false;
  }

  value = 8;

  if (ioctl(m_fd, SPI_IOC_WR_BITS_PER_WORD, &value) == -1)
  {
    LogError("%s: SPI_IOC_WR_BITS_PER_WORD: %s", m_name.c_str(), GetErrno().c_str());
    return false;
  }

  if (ioctl(m_fd, SPI_IOC_RD_BITS_PER_WORD, &value) == -1)
  {
    LogError("%s: SPI_IOC_RD_BITS_PER_WORD: %s", m_name.c_str(), GetErrno().c_str());
    return false;
  }

  value = m_rate;

  if (ioctl(m_fd, SPI_IOC_WR_MAX_SPEED_HZ, &value) == -1)
  {
    LogError("%s: SPI_IOC_WR_MAX_SPEED_HZ: %s", m_name.c_str(), GetErrno().c_str());
    return false;
  }

  if (ioctl(m_fd, SPI_IOC_RD_MAX_SPEED_HZ, &value) == -1)
  {
    LogError("%s: SPI_IOC_RD_MAX_SPEED_HZ: %s", m_name.c_str(), GetErrno().c_str());
    return false;
  }

  if (m_type == LPD8806)
  {
    int latchbytes = (m_channels.size() / 3 + 31) / 32;
    m_buffsize = m_channels.size() + latchbytes;
    m_buff = new uint8_t[m_buffsize];

    //turn of all leds, the LPD8806 needs the high bit set for this
    memset(m_buff, 0x80, m_channels.size());

    //the LPD8806 needs one zero byte per 32 chips (32 rgb leds) to reset the internal counter
    //see the explanation at https://github.com/adafruit/LPD8806/blob/master/LPD8806.cpp
    memset(m_buff + m_channels.size(), 0, latchbytes);

    m_max = 127.0f;
  }
  else if (m_type == WS2801)
  {
    m_buffsize = m_channels.size();
    m_buff = new uint8_t[m_buffsize];
    memset(m_buff, 0, m_buffsize);

    m_max = 255.0f;
  }
  else if (m_type == P9813)
  {
    //P9813 uses 4 bytes per led, plus 4 byte prefix and suffix
    //see: http://www.seeedstudio.com/wiki/images/b/be/P9813_datasheet.pdf
    m_buffsize = ((m_channels.size() / 3) * 4) + 8;
    m_buff = new uint8_t[m_buffsize];
    //Fill the buffer with black
    FillColorP9813(0x00, 0x00, 0x00);

    m_max = 255.0f;
  }

  //write out the buffer to turn off all leds
  if (!WriteBuffer())
    return false;

  return true;
}

bool CDeviceSPI::WriteOutput()
{
  //get the channel values from the clientshandler
  int64_t now = GetTimeUs();
  m_clients.FillChannels(m_channels, now, this);

  if (m_type == LPD8806 || m_type == WS2801)
  {
    //put the values in the buffer, big endian
    for (int i = 0; i < m_channels.size(); i++)
    {
      int output = Round32(m_channels[i].GetValue(now) * m_max);
      m_buff[i] = Clamp(output, 0, Round32(m_max));
    }

    if (m_type == LPD8806)
    {
      //for the LPD8806, high bit needs to be always set
      for (int i = 0; i < m_channels.size(); i++)
        m_buff[i] |= 0x80;
    }
  }
  else if (m_type == P9813)
  {
    memset(m_buff, 0, m_buffsize);
    //put the values in the buffer, big endian
    //i for the channels loop ; j for the buffer loop
    int j = 4;
    for (int i = 0; i < m_channels.size(); i+=3)
    {
        int r = Clamp(Round32(m_channels[i].GetValue(now) * m_max), 0, Round32(m_max));
        int g = Clamp(Round32(m_channels[i+1].GetValue(now) * m_max), 0, Round32(m_max));
        int b = Clamp(Round32(m_channels[i+2].GetValue(now) * m_max), 0, Round32(m_max));

        m_buff[j++] = CalculateChecksumP9813(r, g, b);
        m_buff[j++] = b;
        m_buff[j++] = g;
        m_buff[j++] = r;
    }
  }

  if (!WriteBuffer())
    return false;

  m_timer.Wait();
  
  return true;
}

void CDeviceSPI::CloseDevice()
{
  if (m_fd != -1)
  {
    //turn off all leds
    if (m_type == P9813)
    {
        FillColorP9813(0x00, 0x00, 0x00);
    }
    else
    {
        int value;
        if (m_type == LPD8806)
          value = 0x80;
        else if (m_type == WS2801)
          value = 0;

        memset(m_buff, value, m_channels.size());
    }

    WriteBuffer();

    close(m_fd);
    m_fd = -1;
  }

  delete[] m_buff;
  m_buff = NULL;
  m_buffsize = 0;
}

bool CDeviceSPI::WriteBuffer()
{
  spi_ioc_transfer spi = {};
  spi.tx_buf = (__u64)m_buff;
  spi.len = m_buffsize;

  int returnv = ioctl(m_fd, SPI_IOC_MESSAGE(1), &spi);
  if (returnv == -1)
  {
    LogError("%s: %s %s", m_name.c_str(), m_output.c_str(), GetErrno().c_str());
    return false;
  }

  if (m_debug)
  {
    for (int i = 0; i < m_buffsize; i++)
      printf("%x ", m_buff[i]);

    printf("\n");
  }

  //to latch in the data, the WS2801 needs the clock pin low for 500 microseconds
  if (m_type == WS2801)
    USleep(500);

  return true;
}

uint8_t CDeviceSPI::CalculateChecksumP9813(uint8_t r, uint8_t g, uint8_t b)
{
  uint8_t res = 0;
  //Calculate checksum byte: 1 1 ~b7 ~b6 ~g7 ~g6 ~r7 ~r6
  res |= (uint8_t)0x03 << 6;  
  res |= (uint8_t)(~(b >> 6) & 0x03) << 4; 
  res |= (uint8_t)(~(g >> 6) & 0x03) << 2;    
  res |= (uint8_t)(~(r >> 6) & 0x03); 
  return res;
}

void CDeviceSPI::FillColorP9813(uint8_t r, uint8_t g, uint8_t b)
{
  memset(m_buff, 0, m_buffsize);

  //Write the color in every 4 bytes, except the first and last
  //that should be zero (prefix and suffix) Big endian
  for (int i = 4; i < m_buffsize-4; i += 4) {
      m_buff[i]   = CalculateChecksumP9813(r, g, b);
      m_buff[i+1] = b;
      m_buff[i+2] = g;
      m_buff[i+3] = r;
  }
}
