/*
 *  Copyright (C) 2026 Maxim V.Anisiutkin maxim.anisiutkin@gmail.com
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#define INITGUID

#include "AESinkASIO.h"

#include "cores/AudioEngine/AESinkFactory.h"
#include "cores/AudioEngine/Sinks/windows/AESinkFactoryWin.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "utils/log.h"

#include "platform/win32/CharsetConverter.h"
#include "platform/win32/WIN32Util.h"

#include <algorithm>

CAESinkASIO* CAESinkASIO::ms_this{ nullptr };

CAESinkASIO::CAESinkASIO() :
  m_driver(nullptr),
  m_state(ASIOState::UNLOADED),
  m_nativeDSD(false),
  m_outputDSD(false),
  m_channels(0),
  m_sampleRate(0),
  m_sampleType(-1),
  m_sampleBits(0),
  m_bufferSize(0),
  m_sampleSize(0),
  m_accessSize(0),
  m_frameSize(0),
  m_frameCount(0),
  m_planeBytesPerSec(0),
  m_initialized(false)
{
  (void)CoInitialize(NULL);
}

CAESinkASIO::~CAESinkASIO()
{
  if (m_initialized)
  {
    Deinitialize();
  }
  CoUninitialize();
}

void CAESinkASIO::Register()
{
  AE::AESinkRegEntry reg;
  reg.sinkName = "ASIO";
  reg.createFunc = CAESinkASIO::Create;
  reg.enumerateFunc = CAESinkASIO::EnumerateDevicesEx;
  reg.cleanupFunc = CAESinkASIO::Cleanup;
  AE::CAESinkFactory::RegisterSink(reg);
}

std::unique_ptr<IAESink> CAESinkASIO::Create(std::string& device, AEAudioFormat& desiredFormat)
{
  auto sink = std::make_unique<CAESinkASIO>();
  if (sink->Initialize(desiredFormat, device))
    return sink;
  return {};
}

bool CAESinkASIO::Initialize(AEAudioFormat& format, std::string& device)
{
  CLog::LogF(LOGDEBUG, "CAESinkASIO::Initialize()");
  m_format = format;
  m_device = device;
  if (m_initialized)
    return false;
  m_driver = LoadDriver(device);
  if (!m_driver)
    return false;
  m_state = ASIOState::INITIALIZED;
  m_nativeDSD = false;
  m_sampleRate = m_format.m_sampleRate;
  m_outputDSD = m_format.m_dataFormat == AE_FMT_DSD;
  if (m_outputDSD)
  {
    m_nativeDSD = IsNativeDSDDevice(m_driver);
    if (m_nativeDSD)
    {
      if (m_outputDSD)
      {
        // output as Native DSD
        m_sampleRate *= 8;
        m_format.m_dataFormat = AE_FMT_DSD;
        m_outputDSD = true;
      }
    }
    else
    {
      if (m_outputDSD)
      {
        // output as DoP
        m_sampleRate /= 2;
        m_format.m_sampleRate = m_sampleRate;
        m_format.m_dataFormat = AE_FMT_FLOAT;
        m_outputDSD = false;
      }
    }
  }
  if (m_driver->canSampleRate(m_sampleRate) != ASE_OK)
    return false;
  long numInputChannels, numOutputChannels;
  if (m_driver->getChannels(&numInputChannels, &numOutputChannels) != ASE_OK)
    return false;
  if (numOutputChannels < 1)
    return false;
  m_channels = numOutputChannels;
  m_planeBytesPerSec = m_format.m_sampleRate * (CAEUtil::DataFormatToBits(m_format.m_dataFormat) >> 3);
  m_plane.resize(size_t(ASIO_CACHED_SECONDS * m_planeBytesPerSec));
  if (!m_cache.Create(m_plane.size(), m_channels))
    return false;
  if (m_nativeDSD)
  {
    ASIOIoFormat opt{m_outputDSD ? kASIODSDFormat : kASIOPCMFormat};
    if (m_driver->future(kAsioSetIoFormat, &opt) != ASE_SUCCESS)
    {
      char errorMsg[256]{};
      m_driver->getErrorMessage(errorMsg);
      CLog::LogF(LOGDEBUG, "CAESinkASIO::Initialize() - ASIO future kAsioSetIoFormat error: {}", errorMsg);
      return false;
    }
  }
  if (m_driver->setSampleRate(m_sampleRate) != ASE_OK)
    return false;
  long minSize, maxSize, preferredSize, granularity;
  if (m_driver->getBufferSize(&minSize, &maxSize, &preferredSize, &granularity) != ASE_OK)
    return false;
  ASIOChannelInfo info{0, ASIOFalse};
  if (m_driver->getChannelInfo(&info) != ASE_OK)
    return false;
  m_sampleType = info.type;
  m_sampleBits = GetASIOSampleSizeInBits(m_sampleType);
  m_bufferSize = preferredSize;
  m_sampleSize = (m_sampleBits + 7) / 8;
  m_accessSize = (m_bufferSize * m_sampleBits) / 8;
  m_bufferInfos.resize(m_channels);
  for (long ch = 0; ch < m_channels; ch++)
  {
    m_bufferInfos[ch].isInput = ASIOFalse;
    m_bufferInfos[ch].channelNum = ch;
  }
  if (!CreateBuffers())
    return false;
  m_format.m_frameSize = m_format.m_channelLayout.Count() * (CAEUtil::DataFormatToBits(m_format.m_dataFormat) >> 3);
  m_format.m_frames = m_accessSize / m_sampleSize;
  format = m_format;
  m_initialized = true;
  return true;
}

void CAESinkASIO::Deinitialize()
{
  CLog::LogF(LOGDEBUG, "CAESinkASIO::Deinitialize()");
  if (!m_initialized)
    return;
  if (m_driver)
  {
    Stop();
    DisposeBuffers();
    ms_this = nullptr;
    m_driver->Release();
    m_driver = nullptr;
    m_state = ASIOState::UNLOADED;
  }
  m_initialized = false;
}

double CAESinkASIO::GetCacheTotal()
{
  return (double)m_cache.GetMaxSize() / (double)m_planeBytesPerSec;
}

double CAESinkASIO::GetLatency()
{
  if (m_driver)
  {
    long inputLatency, outputLatency;
    if (m_driver->getLatencies(&inputLatency, &outputLatency) == ASE_OK)
    {
      if (m_sampleRate > 0)
      {
        return outputLatency / m_sampleRate;
      }
    }
  }
  return 0.0;
}

unsigned int CAESinkASIO::AddPackets(uint8_t** data, unsigned int frames, unsigned int offset)
{
  if (!m_initialized)
    return 0U;
  std::unique_lock lk(m_mutex);
  m_cv.wait(lk, [this, frames] { return frames <= m_cache.GetWriteSize() / m_sampleSize; });
  auto isPlanar{ AE_IS_PLANAR(m_format.m_dataFormat) };
  auto inpSampleSize = CAEUtil::DataFormatToBits(m_format.m_dataFormat) >> 3;
  for (auto ch = 0U; ch < std::min(m_format.m_channelLayout.Count(), m_cache.NumPlanes()); ch++)
  {
    for (auto sample = offset; sample < frames; sample++)
    {
      ConvertSample(m_plane.data() + sample * m_sampleSize,
        m_sampleType,
        data[isPlanar ? ch : 0] + (offset + sample) * m_format.m_frameSize + (!isPlanar ? ch : 0) * inpSampleSize,
        m_format.m_dataFormat
      ); 
    }
    m_cache.Write(m_plane.data(), frames * m_sampleSize, ch);
  }
  for (auto ch = std::min(m_format.m_channelLayout.Count(), m_cache.NumPlanes()); ch < m_cache.NumPlanes(); ch++)
  {
    ZeroSamples(m_plane.data(), m_sampleType, frames);
    m_cache.Write(m_plane.data(), frames * m_sampleSize, ch);
  }
  if (m_state != ASIOState::RUNNING && m_cache.GetReadSize() >= m_cache.GetMaxSize() / 2)
  {
    Start();
  }
  return frames;
}

void CAESinkASIO::AddPause(unsigned int millis)
{
}

void CAESinkASIO::GetDelay(AEDelayStatus& status)
{
  if (!m_initialized)
  {
    status.SetDelay(0);
    return;
  }
  status.SetDelay((double)m_cache.GetReadSize() / (double)m_planeBytesPerSec + GetLatency());
}

void CAESinkASIO::Drain()
{
  if (!m_initialized)
    return;
  Stop();
  m_cache.Dump();
}

bool CAESinkASIO::HasVolume()
{
  return true;
}

void CAESinkASIO::SetVolume(float volume)
{
}

bool CAESinkASIO::CreateBuffers()
{
  if (m_state == ASIOState::INITIALIZED)
  {
    ms_this = this;
    ASIOCallbacks callbacks{s_bufferSwitch, s_sampleRateDidChange, s_asioMessage, s_bufferSwitchTimeInfo};
    auto err = m_driver->createBuffers(m_bufferInfos.data(), m_channels, m_bufferSize, &callbacks);
    if (err == ASE_OK)
      m_state = ASIOState::PREPARED;
    else
      CLog::LogF(LOGDEBUG, "CAESinkASIO::CreateBuffers() - ASIO create buffers error {}", err);
  }
  return m_state == ASIOState::PREPARED;
}

bool CAESinkASIO::DisposeBuffers()
{
  if (m_state == ASIOState::PREPARED)
  {
    auto err = m_driver->disposeBuffers();
    if (err == ASE_OK)
      m_state = ASIOState::INITIALIZED;
    else
      CLog::LogF(LOGDEBUG, "CAESinkASIO::DisposeBuffers() - ASIO dispose buffers error {}", err);
  }
  ms_this = nullptr;
  return m_state == ASIOState::INITIALIZED;
}

bool CAESinkASIO::Start()
{
  if (m_state == ASIOState::PREPARED)
  {
    auto err = m_driver->start();
    if (err == ASE_OK)
      m_state = ASIOState::RUNNING;
    else
      CLog::LogF(LOGDEBUG, "CAESinkASIO::Start() - ASIO start error {}", err);
  }
  return false;
}

bool CAESinkASIO::Stop()
{
  if (m_state == ASIOState::RUNNING)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto err = m_driver->stop();
    if (err == ASE_OK)
      m_state = ASIOState::PREPARED;
    else
      CLog::LogF(LOGDEBUG, "CAESinkASIO::Stop() - ASIO stop error {}", err);
    m_cv.notify_one();
  }
  return m_state == ASIOState::PREPARED;
}

void CAESinkASIO::EnumerateDevicesEx(AEDeviceInfoList& deviceInfoList, bool force)
{
  HKEY hAsioKey;
  if (RegOpenKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", &hAsioKey) == ERROR_SUCCESS)
  {
    if (SUCCEEDED(CoInitialize(NULL)))
    {
      DWORD devIndex{ 0U };
      for (;;)
      {
        WCHAR devName[256]{};
        DWORD devNameLen = DWORD(sizeof(devName) / sizeof(WCHAR));
        if (RegEnumKeyExW(hAsioKey, devIndex++, devName, &devNameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
        {
          break;
        }
        HKEY hDevKey;
        if (RegOpenKeyExW(hAsioKey, devName, 0, KEY_READ, &hDevKey) == ERROR_SUCCESS)
        {
          WCHAR devClsId[256]{};
          DWORD devClsIdLen = sizeof(devClsId);
          if (RegQueryValueExW(hDevKey, L"CLSID", 0, NULL, (LPBYTE)devClsId, &devClsIdLen) == ERROR_SUCCESS)
          {
            if (devClsIdLen > 0)
            {
              CLSID clsId;
              if (SUCCEEDED(CLSIDFromString((LPOLESTR)devClsId, &clsId)))
              {
                CAEDeviceInfo deviceInfo{};
                deviceInfo.m_deviceName = KODI::PLATFORM::WINDOWS::FromW(devClsId, devClsIdLen / sizeof(WCHAR) - 1);
                WCHAR devDesc[256]{};
                DWORD devDescLen = DWORD(sizeof(devDesc) / sizeof(WCHAR));
                if (RegQueryValueExW(hDevKey, L"Description", 0, NULL, (LPBYTE)devDesc, &devDescLen) == ERROR_SUCCESS)
                {
                  deviceInfo.m_displayName = KODI::PLATFORM::WINDOWS::FromW(devDesc, devDescLen / sizeof(WCHAR) - 1);
                }
                else
                {
                  deviceInfo.m_displayName = KODI::PLATFORM::WINDOWS::FromW(devName, devNameLen / sizeof(WCHAR));
                }
                deviceInfo.m_deviceType = AE_DEVTYPE_PCM;
                deviceInfo.m_wantsIECPassthrough = false;
                auto driver = LoadDriver(deviceInfo.m_deviceName);
                if (driver)
                {
                  long numInputChannels, numOutputChannels;
                  if (driver->getChannels(&numInputChannels, &numOutputChannels) == ASE_OK)
                  {
                    deviceInfo.m_channels = layoutsByChCount[std::max(std::min(unsigned int(numOutputChannels), ASIO_MAX_CHANNEL_COUNT), 2U)];
                  }
                  if (IsNativeDSDDevice(driver))
                  {
                    for (auto i = 0; i < 5; i++)
                    {
                      unsigned samplerate;
                      samplerate = 64 * 44100 * (1 << i);
                      if (driver->canSampleRate(ASIOSampleRate(samplerate)) == ASE_OK)
                      {
                        deviceInfo.m_sampleRates.push_back(samplerate);
                      }
                      samplerate = 64 * 48000 * (1 << i);
                      if (driver->canSampleRate(ASIOSampleRate(samplerate)) == ASE_OK)
                      {
                        deviceInfo.m_sampleRates.push_back(samplerate);
                      }
                    }
                    // signal that device is capable of playing Native DSD 
                    deviceInfo.m_dataFormats.push_back(AE_FMT_DSD);
                  }
                  for (auto i = 0; i < 5; i++)
                  {
                    unsigned int samplerate;
                    samplerate = 44100 * (1 << i);
                    if (driver->canSampleRate(ASIOSampleRate(samplerate)) == ASE_OK)
                    {
                      deviceInfo.m_sampleRates.push_back(samplerate);
                    }
                    samplerate = 48000 * (1 << i);
                    if (driver->canSampleRate(ASIOSampleRate(samplerate)) == ASE_OK)
                    {
                      deviceInfo.m_sampleRates.push_back(samplerate);
                    }
                  }
                  ASIOChannelInfo info{ 0, ASIOFalse };
                  if (driver->getChannelInfo(&info) == ASE_OK)
                  {
                    deviceInfo.m_dataFormats.push_back(GetAEDataFormatForASIOSampleType(info.type));
                  }
                  deviceInfo.m_dataFormats.push_back(AE_FMT_FLOAT);
                  driver->Release();
                }
                deviceInfoList.push_back(deviceInfo);
              }
            }
            RegCloseKey(hDevKey);
          }
        }
      }
      CoUninitialize();
    }
    RegCloseKey(hAsioKey);
  }
}

void CAESinkASIO::Cleanup()
{
  CLog::LogF(LOGDEBUG, "CAESinkASIO::Cleanup()");
}

///////////////////////////////////////////////////////////////////////////////

unsigned int CAESinkASIO::GetASIOSampleSizeInBits(ASIOSampleType sampleType)
{
  switch (sampleType) {
  case ASIOSTDSDInt8MSB1:
  case ASIOSTDSDInt8LSB1:
	  return 1;
  case ASIOSTDSDInt8NER8:
	  return 8;
  case ASIOSTInt16MSB:
  case ASIOSTInt16LSB:
	  return 16;
  case ASIOSTInt24MSB:
  case ASIOSTInt24LSB:
	  return 24;
  case ASIOSTInt32MSB:
  case ASIOSTInt32MSB16:
  case ASIOSTInt32MSB18:
  case ASIOSTInt32MSB20:
  case ASIOSTInt32MSB24:
  case ASIOSTInt32LSB:
  case ASIOSTInt32LSB16:
  case ASIOSTInt32LSB18:
  case ASIOSTInt32LSB20:
  case ASIOSTInt32LSB24:
  case ASIOSTFloat32MSB:
  case ASIOSTFloat32LSB:
	  return 32;
  case ASIOSTFloat64MSB:
  case ASIOSTFloat64LSB:
  	return 64;
  default:
	  return 0;
  }
}

AEDataFormat CAESinkASIO::GetAEDataFormatForASIOSampleType(ASIOSampleType sampleType)
{
	switch (sampleType) {
	case ASIOSTInt16MSB:
		return AE_FMT_S16BE;
	case ASIOSTInt24MSB:
		return AE_FMT_S24BE3;
	case ASIOSTInt32MSB:
		return AE_FMT_S32BE;
	case ASIOSTFloat32MSB:
		return AE_FMT_FLOAT;
	case ASIOSTFloat64MSB:
		return AE_FMT_DOUBLE;
	case ASIOSTInt32MSB16:
		return AE_FMT_S32BE;
	case ASIOSTInt32MSB18:
		return AE_FMT_S32BE;
	case ASIOSTInt32MSB20:
		return AE_FMT_S32BE;
	case ASIOSTInt32MSB24:
		return AE_FMT_S32BE;
	case ASIOSTInt16LSB:
		return AE_FMT_S16LE;
	case ASIOSTInt24LSB:
		return AE_FMT_S24LE3;
	case ASIOSTInt32LSB:
		return AE_FMT_S32LE;
	case ASIOSTFloat32LSB:
		return AE_FMT_FLOAT;
	case ASIOSTFloat64LSB:
		return AE_FMT_DOUBLE;
	case ASIOSTInt32LSB16:
		return AE_FMT_S32LE;
	case ASIOSTInt32LSB18:
		return AE_FMT_S32LE;
	case ASIOSTInt32LSB20:
		return AE_FMT_S32LE;
	case ASIOSTInt32LSB24:
		return AE_FMT_S32LE;
	case ASIOSTDSDInt8LSB1:
		return AE_FMT_DSD;
	case ASIOSTDSDInt8MSB1:
		return AE_FMT_DSD;
	case ASIOSTDSDInt8NER8:
		return AE_FMT_INVALID;
	default:
		return AE_FMT_INVALID;
	}
}

template<typename real_t, typename int_t>
int_t ConvertRealToInt(real_t inp_value)
{
	constexpr real_t scale = 1ul << ((sizeof(int_t) << 3) - 1);
	constexpr auto min_value = std::numeric_limits<int_t>::min();
	constexpr auto max_value = std::numeric_limits<int_t>::max();
	auto int_value = std::llround(inp_value * scale);
	int_t out_value;
	if (int_value < min_value) {
		out_value = min_value;
	}
	else {
		if (int_value < max_value) {
			out_value = int_t(int_value);
		}
		else {
			out_value = max_value;
		}
	}
	return out_value;
}

void CAESinkASIO::ConvertSample(void* outValue, ASIOSampleType outType, const void* inpValue, AEDataFormat inpType)
{
  auto v = const_cast<void*>(inpValue);
  switch (inpType)
  {
  case AE_FMT_DSD:
    switch (outType)
    {
    case ASIOSTDSDInt8LSB1:
      {
        auto value = *reinterpret_cast<uint8_t*>(v);
        reinterpret_cast<uint8_t*>(outValue)[0] = REVERSE_BIT_TABLE[value];
      }
      break;
    case ASIOSTDSDInt8MSB1:
    {
      auto value = *reinterpret_cast<uint8_t*>(v);
      reinterpret_cast<uint8_t*>(outValue)[0] = value;
    }
    break;
  }
  break;
  case AE_FMT_FLOAT:
    switch (outType)
    {
    case ASIOSTInt16MSB:
      {
        auto value = ConvertRealToInt<float, short>(*reinterpret_cast<float*>(v));
        *reinterpret_cast<short*>(outValue) = _byteswap_ushort(value);
      }
      break;
    case ASIOSTInt16LSB:
      {
        auto value = ConvertRealToInt<float, short>(*reinterpret_cast<float*>(v));
        *reinterpret_cast<short*>(outValue) = value;
      }
      break;
    case ASIOSTInt24MSB:
      {
        auto value = ConvertRealToInt<float, int>(*reinterpret_cast<float*>(v));
        reinterpret_cast<uint8_t*>(outValue)[0] = reinterpret_cast<uint8_t*>(&value)[3];
        reinterpret_cast<uint8_t*>(outValue)[1] = reinterpret_cast<uint8_t*>(&value)[2];
        reinterpret_cast<uint8_t*>(outValue)[2] = reinterpret_cast<uint8_t*>(&value)[1];
      }
      break;
    case ASIOSTInt24LSB:
      {
        auto value = ConvertRealToInt<float, int>(*reinterpret_cast<float*>(v));
        reinterpret_cast<uint8_t*>(outValue)[0] = reinterpret_cast<uint8_t*>(&value)[1];
        reinterpret_cast<uint8_t*>(outValue)[1] = reinterpret_cast<uint8_t*>(&value)[2];
        reinterpret_cast<uint8_t*>(outValue)[2] = reinterpret_cast<uint8_t*>(&value)[3];
      }
      break;
    case ASIOSTInt32MSB:
    case ASIOSTInt32MSB16:
    case ASIOSTInt32MSB18:
    case ASIOSTInt32MSB20:
    case ASIOSTInt32MSB24:
      {
        auto value = ConvertRealToInt<float, int>(*reinterpret_cast<float*>(v));
        *reinterpret_cast<int*>(outValue) = _byteswap_ulong(value);
      }
      break;
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
      {
        auto value = ConvertRealToInt<float, int>(*reinterpret_cast<float*>(v));
        *reinterpret_cast<int*>(outValue) = value;
      }
      break;
    case ASIOSTFloat32MSB:
      {
        auto value = (float)*reinterpret_cast<float*>(v);
        *reinterpret_cast<uint32_t*>(outValue) = _byteswap_ulong(*reinterpret_cast<uint32_t*>(&value));
      }
      break;
    case ASIOSTFloat64MSB:
      {
        auto value = (double)*reinterpret_cast<float*>(v);
        *reinterpret_cast<uint64_t*>(outValue) = _byteswap_uint64(*reinterpret_cast<uint64_t*>(&value));
      }
      break;
    case ASIOSTFloat32LSB:
      {
        auto value = (float)*reinterpret_cast<float*>(v);
        *reinterpret_cast<uint32_t*>(outValue) = *reinterpret_cast<uint32_t*>(&value);
      }
      break;
    case ASIOSTFloat64LSB:
      {
        auto value = (double)*reinterpret_cast<float*>(v);
        *reinterpret_cast<uint64_t*>(outValue) = *reinterpret_cast<uint64_t*>(&value);
      }
      break;
    case ASIOSTDSDInt8LSB1:
      {
        auto value = static_cast<int>(*reinterpret_cast<float*>(v) * 0x80000000);
        reinterpret_cast<uint8_t*>(outValue)[0] = REVERSE_BIT_TABLE[static_cast<uint8_t>(value >> 16)];
        reinterpret_cast<uint8_t*>(outValue)[1] = REVERSE_BIT_TABLE[static_cast<uint8_t>(value >> 8)];
      }
      break;
    case ASIOSTDSDInt8MSB1:
      {
        auto value = static_cast<int>(*reinterpret_cast<float*>(v) * 0x80000000);
        reinterpret_cast<uint8_t*>(outValue)[0] = static_cast<uint8_t>(value >> 16);
        reinterpret_cast<uint8_t*>(outValue)[1] = static_cast<uint8_t>(value >> 8);
      }
      break;
    }
    break;
  case AE_FMT_DOUBLE:
    switch (outType)
    {
    case ASIOSTInt16MSB:
      {
        auto value = ConvertRealToInt<double, short>(*reinterpret_cast<double*>(v));
        *reinterpret_cast<short*>(outValue) = _byteswap_ushort(value);
      }
      break;
    case ASIOSTInt16LSB:
      {
        auto value = ConvertRealToInt<double, short>(*reinterpret_cast<double*>(v));
        *reinterpret_cast<short*>(outValue) = value;
      }
      break;
    case ASIOSTInt24MSB:
      {
        auto value = ConvertRealToInt<double, int>(*reinterpret_cast<double*>(v));
        reinterpret_cast<uint8_t*>(outValue)[0] = reinterpret_cast<uint8_t*>(&value)[3];
        reinterpret_cast<uint8_t*>(outValue)[1] = reinterpret_cast<uint8_t*>(&value)[2];
        reinterpret_cast<uint8_t*>(outValue)[2] = reinterpret_cast<uint8_t*>(&value)[1];
      }
      break;
    case ASIOSTInt24LSB:
      {
        auto value = ConvertRealToInt<double, int>(*reinterpret_cast<double*>(v));
        reinterpret_cast<uint8_t*>(outValue)[0] = reinterpret_cast<uint8_t*>(&value)[1];
        reinterpret_cast<uint8_t*>(outValue)[1] = reinterpret_cast<uint8_t*>(&value)[2];
        reinterpret_cast<uint8_t*>(outValue)[2] = reinterpret_cast<uint8_t*>(&value)[3];
      }
      break;
    case ASIOSTInt32MSB:
    case ASIOSTInt32MSB16:
    case ASIOSTInt32MSB18:
    case ASIOSTInt32MSB20:
    case ASIOSTInt32MSB24:
      {
        auto value = ConvertRealToInt<double, int>(*reinterpret_cast<double*>(v));
        *reinterpret_cast<int*>(outValue) = _byteswap_ulong(value);
      }
      break;
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
      {
        auto value = ConvertRealToInt<double, int>(*reinterpret_cast<double*>(v));
        *reinterpret_cast<int*>(outValue) = value;
      }
      break;
    case ASIOSTFloat32MSB:
      {
        auto value = (float)*reinterpret_cast<double*>(v);
        *reinterpret_cast<uint32_t*>(outValue) = _byteswap_ulong(*reinterpret_cast<uint32_t*>(&value));
      }
      break;
    case ASIOSTFloat64MSB:
      {
        auto value = (double)*reinterpret_cast<double*>(v);
        *reinterpret_cast<uint64_t*>(outValue) = _byteswap_uint64(*reinterpret_cast<uint64_t*>(&value));
      }
      break;
    case ASIOSTFloat32LSB:
      {
        auto value = (float)*reinterpret_cast<double*>(v);
        *reinterpret_cast<uint32_t*>(outValue) = *reinterpret_cast<uint32_t*>(&value);
      }
      break;
    case ASIOSTFloat64LSB:
      {
        auto value = (double)*reinterpret_cast<double*>(v);
        *reinterpret_cast<uint64_t*>(outValue) = *reinterpret_cast<uint64_t*>(&value);
      }
      break;
    }
    break;
  }
}

void CAESinkASIO::ZeroSamples(void* data, ASIOSampleType type, unsigned int samples)
{
  switch (type)
  {
  case ASIOSTDSDInt8MSB1:
  case ASIOSTDSDInt8LSB1:
    std::memset(data, DSD_SILENCE_BYTE, samples);
    break;
  case ASIOSTDSDInt8NER8:
    for (auto sample = 0U; sample < samples; sample++)
      ((uint8_t*)data)[sample] = (DSD_SILENCE_BYTE >> (7 - sample % 8)) & 1;
    break;
  default:
    std::memset(data, 0, samples * (GetASIOSampleSizeInBits(type) >> 3));
    break;
  }
}

IASIO* CAESinkASIO::LoadDriver(std::string& device)
{
  IASIO* driver{ nullptr };
  CLSID clsId;
  if (SUCCEEDED(CLSIDFromString((LPOLESTR)KODI::PLATFORM::WINDOWS::ToW(device).c_str(), &clsId)))
  {
    try
    {
      if (SUCCEEDED(CoCreateInstance(clsId, nullptr, CLSCTX_INPROC_SERVER, clsId, (LPVOID*)&driver)))
      {
        if (driver->init(nullptr) != ASIOTrue)
        {
          driver->Release();
          driver = nullptr;
        }
      }
    }
    catch (...)
    {
      driver = nullptr;
    }
    if (driver)
    {
      long version{};
      char name[32]{};
      version = driver->getDriverVersion();
      driver->getDriverName(name);
    }
  }
  return driver;
}

bool CAESinkASIO::IsNativeDSDDevice(IASIO* driver)
{
  ASIOIoFormat opt{kASIODSDFormat};
  return driver->future(kAsioCanDoIoFormat, &opt) == ASE_SUCCESS;
}

// ASIO callbacks

void CAESinkASIO::s_bufferSwitch(long doubleBufferIndex, ASIOBool directProcess)
{
  ms_this->bufferSwitch(nullptr, doubleBufferIndex, directProcess);
}

void CAESinkASIO::s_sampleRateDidChange(ASIOSampleRate sampleRate)
{
  ms_this->sampleRateDidChange(sampleRate);
}

long CAESinkASIO::s_asioMessage(long selector, long value, void* message, double* opt)
{
  return ms_this->asioMessage(selector, value, message, opt);
}

ASIOTime* CAESinkASIO::s_bufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess)
{
  ms_this->bufferSwitch(params, doubleBufferIndex, directProcess);
  return params;
}

void CAESinkASIO::bufferSwitch(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_accessSize <= m_cache.GetReadSize())
  {
    for (auto ch = 0; ch < m_channels; ch++)
    {
      m_cache.Read((uint8_t*)m_bufferInfos[ch].buffers[doubleBufferIndex], m_accessSize, ch);
    }
  }
  else
  {
    for (auto ch = 0; ch < m_channels; ch++)
    {
      ZeroSamples(m_bufferInfos[ch].buffers[doubleBufferIndex], m_sampleType, m_accessSize / m_sampleSize);
    }
  }
  m_cv.notify_one();
}

void CAESinkASIO::sampleRateDidChange(ASIOSampleRate sRate)
{
}

long CAESinkASIO::asioMessage(long selector, long value, void* message, double* opt)
{
  long v{ 0L };
  switch (selector)
  {
    case kAsioSelectorSupported:
      switch (value)
      {
      case kAsioSelectorSupported:
      case kAsioEngineVersion:
      case kAsioResetRequest:
      case kAsioSupportsTimeInfo:
        v = 1L;
        break;
      }
      break;
    case kAsioEngineVersion:
      v = 2L;
      break;
    case kAsioResetRequest:
      v = 1L;
      break;
    case kAsioSupportsTimeInfo:
      v = 1L;
      break;
  }
  return v;
}
