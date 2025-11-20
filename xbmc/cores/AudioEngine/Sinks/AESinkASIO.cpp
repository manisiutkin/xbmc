/*
 *  Copyright (C) 2025 Maxim V.Anisiutkin maxim.anisiutkin@gmail.com
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#define INITGUID


#include "AESinkASIO.h"

#include "cores/AudioEngine/AESinkFactory.h"
#include "cores/AudioEngine/Sinks/windows/AESinkFactoryWin.h"
#include "cores/AudioEngine/Utils/AERingBuffer.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "utils/StringUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"

#include "platform/win32/CharsetConverter.h"
#include "platform/win32/WIN32Util.h"

#include <algorithm>
#include <mutex>

#include <Mmreg.h>
#include <initguid.h>

// include order is important here
// clang-format off
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
// clang-format on


CAESinkASIO* CAESinkASIO::s_this{ nullptr };


extern HWND g_hWnd;

DEFINE_GUID( _KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, WAVE_FORMAT_IEEE_FLOAT, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 );
DEFINE_GUID( _KSDATAFORMAT_SUBTYPE_DOLBY_AC3_SPDIF, WAVE_FORMAT_DOLBY_AC3_SPDIF, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 );

#define EXIT_ON_FAILURE(hr, reason) \
  if (FAILED(hr)) \
  { \
    CLog::LogF(LOGERROR, reason " - error {}", hr, CWIN32Util::FormatHRESULT(hr)); \
    goto failed; \
  }

namespace
{
constexpr unsigned int ASIO_MAX_CHANNEL_COUNT = 8U;
constexpr unsigned int DSChannelOrder[] = {
    SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER, SPEAKER_LOW_FREQUENCY,
    SPEAKER_BACK_LEFT,  SPEAKER_BACK_RIGHT,  SPEAKER_SIDE_LEFT,    SPEAKER_SIDE_RIGHT};
constexpr enum AEChannel AEChannelNamesDS[] = {AE_CH_FL, AE_CH_FR, AE_CH_FC, AE_CH_LFE, AE_CH_BL,
                                               AE_CH_BR, AE_CH_SL, AE_CH_SR, AE_CH_NULL};
} // namespace

using namespace Microsoft::WRL;

CAESinkASIO::CAESinkASIO() :
  m_iasio(nullptr),
  m_planeCount(0),
  m_sampleType(-1),
  m_sampleSize(0),
  m_bufferSize(0),
  m_frameSize(0),
  m_frameCount(0),
  m_planeBytesPerSec(0),
  m_initialized(false),
  m_running(false)
{
}

CAESinkASIO::~CAESinkASIO()
{
  Deinitialize();
}

void CAESinkASIO::Register()
{
  AE::AESinkRegEntry reg;
  reg.sinkName = "ASIO";
  reg.createFunc = CAESinkASIO::Create;
  reg.enumerateFunc = CAESinkASIO::EnumerateDevicesEx;
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
  if (m_initialized)
    return false;
  if (FAILED(CoInitialize(NULL)))
    return false;
  m_iasio = Load(device);
  if (!m_iasio)
    return false;
  if (format.m_sampleRate >= DSD_MIN_SAMPLERATE)
  {
    ASIOIoFormat opt{ kASIODSDFormat };
    if (m_iasio->future(kAsioSetIoFormat, &opt) != ASE_SUCCESS)
      return false;
  }
  if (m_iasio->canSampleRate(ASIOSampleRate(format.m_sampleRate)) != ASE_OK)
    return false;
  if (m_iasio->setSampleRate(ASIOSampleRate(format.m_sampleRate)) != ASE_OK)
    return false;
  long numInputChannels, numOutputChannels;
  if (m_iasio->getChannels(&numInputChannels, &numOutputChannels) != ASE_OK)
    return false;
  m_planeCount = numOutputChannels;
  long minSize, maxSize, preferredSize, granularity;
  if (m_iasio->getBufferSize(&minSize, &maxSize, &preferredSize, &granularity) != ASE_OK)
    return false;
  m_bufferSize = preferredSize;
  ASIOChannelInfo info{ 0, ASIOFalse };
  if (m_iasio->getChannelInfo(&info) != ASE_OK)
    return false;
  m_sampleType = info.type;
  m_sampleSize = (GetASIOSampleSizeInBits(m_sampleType) + 7) / 8;
  s_this = this;
  m_bufferInfos.resize(m_planeCount);
  for (long plane = 0; plane < m_planeCount; plane++)
  {
    m_bufferInfos[plane].isInput = ASIOFalse;
    m_bufferInfos[plane].channelNum = plane;
  }
  ASIOCallbacks callbacks{ s_bufferSwitch, s_sampleRateDidChange, s_asioMessage, s_bufferSwitchTimeInfo };
  if (m_iasio->createBuffers(m_bufferInfos.data(), m_planeCount, m_bufferSize, &callbacks) != ASE_OK)
    return false;
  
  m_frameSize = format.m_channelLayout.Count() * (CAEUtil::DataFormatToBits(format.m_dataFormat) / 8U);
  m_frameCount = format.m_sampleRate / ((format.m_sampleRate >= DSD_MIN_SAMPLERATE) ? 8 : 1)  / 75U;
  format.m_frameSize =  m_frameSize;
  format.m_frames = m_frameCount;

  m_planeBytesPerSec = format.m_sampleRate * GetASIOSampleSizeInBits(m_sampleType);
  auto frameBytes = m_planeBytesPerSec / 75U;
  if (!m_planeBuffer.Create(frameBytes * 3U * 75U, m_planeCount))
    return false;

  m_format = format;
  m_initialized = true;
  if (m_iasio->start() != ASE_OK)
    return false;
  m_running = true;
  return true;
}

void CAESinkASIO::Deinitialize()
{
  if (m_iasio)
  {
    m_iasio->stop();
    m_running = false;
    m_iasio->disposeBuffers();
    s_this = nullptr;
    m_iasio->Release();
    m_iasio = nullptr;
  }
  CoUninitialize();
  if (!m_initialized)
    return;
  m_initialized = false;
  CLog::LogF(LOGDEBUG, "CAESinkASIO::Deinitialize()");
}

unsigned int CAESinkASIO::AddPackets(uint8_t **data, unsigned int frames, unsigned int offset)
{
  if (!m_initialized)
    return 0;

  auto dataPtr = (uint8_t*)data[0] + offset * m_format.m_frameSize;
  auto frameChannels = m_format.m_channelLayout.Count();
  auto planesToConsume = std::min(m_planeBuffer.NumPlanes(), frameChannels);
  auto framesToConsume = std::min(frames, m_planeBuffer.GetWriteSize() / m_sampleSize);
  m_planePad.resize(framesToConsume * m_sampleSize);
  for (auto plane = 0U; plane < planesToConsume; plane++)
  {
    ConvertSamples(m_planePad.data(), m_sampleType, m_sampleSize, dataPtr, plane, frameChannels, m_format.m_dataFormat, m_frameSize / frameChannels, framesToConsume);
    m_planeBuffer.Write(m_planePad.data(), framesToConsume * m_sampleSize);
  }
  if (planesToConsume < m_planeBuffer.NumPlanes())
  {
    ZeroSamples(m_planePad.data(), m_sampleType, m_sampleSize, framesToConsume);
    for (auto plane = planesToConsume; plane < m_planeBuffer.NumPlanes(); plane++)
      m_planeBuffer.Write(m_planePad.data(), framesToConsume * m_sampleSize);
  }
  return framesToConsume;
}

void CAESinkASIO::Stop()
{
  m_iasio->stop();
  m_running = false;
}

void CAESinkASIO::Drain()
{
  if (!m_initialized)
    return;

  if (m_running)
  {
    m_iasio->stop();
    m_running = false;
  }
  m_running = false;
  m_planeBuffer.Dump();
}

void CAESinkASIO::GetDelay(AEDelayStatus& status)
{
  if (!m_initialized)
  {
    status.SetDelay(0);
    return;
  }
  status.SetDelay((double)m_planeBuffer.GetReadSize() / (double)m_planeBytesPerSec);
}

double CAESinkASIO::GetCacheTotal()
{
  return (double)m_planeBuffer.GetMaxSize() / (double)m_planeBytesPerSec;
}

void CAESinkASIO::EnumerateDevicesEx(AEDeviceInfoList &deviceInfoList, bool force)
{
  HKEY hAsioKey;
  if (RegOpenKeyW(HKEY_LOCAL_MACHINE, L"software\\asio", &hAsioKey) == ERROR_SUCCESS)
  {
    if (SUCCEEDED(CoInitialize(NULL)))
    {
      DWORD devIndex{ 0 };
      for (;;)
      {
        WCHAR devName[256]{};
        DWORD devNameLen = DWORD(sizeof(devName) / sizeof(WCHAR));
        if (RegEnumKeyExW(hAsioKey, devIndex++, devName, &devNameLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
          break;
        HKEY hDevKey;
        if (RegOpenKeyExW(hAsioKey, devName, 0, KEY_READ, &hDevKey) == ERROR_SUCCESS)
        {
          WCHAR devClsId[256]{};
          DWORD devClsIdLen = sizeof(devClsId);
          if (RegQueryValueExW(hDevKey, L"clsid", 0, NULL, (LPBYTE)devClsId, &devClsIdLen) == ERROR_SUCCESS)
          {
            if (devClsIdLen > 0)
            {
              CLSID clsId;
              if (SUCCEEDED(CLSIDFromString((LPOLESTR)devClsId, &clsId)))
              {
                CAEDeviceInfo deviceInfo{};
                deviceInfo.m_deviceName = KODI::PLATFORM::WINDOWS::FromW(devClsId, devClsIdLen / sizeof(WCHAR));
                WCHAR devDesc[256]{};
                DWORD devDescLen = DWORD(sizeof(devDesc) / sizeof(WCHAR));
                if (RegQueryValueExW(hDevKey, L"description", 0, NULL, (LPBYTE)devDesc, &devDescLen) == ERROR_SUCCESS)
                  deviceInfo.m_displayName = KODI::PLATFORM::WINDOWS::FromW(devDesc, devDescLen / sizeof(WCHAR));
                else
                  deviceInfo.m_displayName = KODI::PLATFORM::WINDOWS::FromW(devName, devNameLen / sizeof(WCHAR));
                deviceInfo.m_deviceType = AE_DEVTYPE_PCM;
                deviceInfo.m_wantsIECPassthrough = true;
                auto iasio = Load(deviceInfo.m_deviceName);
                if (iasio)
                {
                  long numInputChannels, numOutputChannels;
	                if (iasio->getChannels(&numInputChannels, &numOutputChannels) == ASE_OK)
                    deviceInfo.m_channels = layoutsByChCount[std::max(std::min(unsigned(numOutputChannels), ASIO_MAX_CHANNEL_COUNT), 2U)];
                  for (auto i = 0; i < 5; i++)
                  {
                    unsigned samplerate;
                    samplerate = 44100 * (1 << i);
                    if (iasio->canSampleRate(ASIOSampleRate(samplerate)) == ASE_OK)
                      deviceInfo.m_sampleRates.push_back(samplerate);
                    samplerate = 48000 * (1 << i);
                    if (iasio->canSampleRate(ASIOSampleRate(samplerate)) == ASE_OK)
                      deviceInfo.m_sampleRates.push_back(samplerate);
                  }
                  ASIOChannelInfo info{ 0, ASIOFalse };
	                if (iasio->getChannelInfo(&info) == ASE_OK)
                    deviceInfo.m_dataFormats.push_back(GetAEDataFormatForASIOSampleType(info.type));
                  ASIOIoFormat opt;
                  opt.FormatType = kASIODSDFormat;
                  if (iasio->future(kAsioSetIoFormat, &opt) == ASE_SUCCESS)
                  {
                    for (auto i = 0; i < 5; i++)
                    {
                      unsigned samplerate;
                      samplerate = 64 * 44100 * (1 << i);
                      if (iasio->canSampleRate(ASIOSampleRate(samplerate)) == ASE_OK)
                        deviceInfo.m_sampleRates.push_back(samplerate);
                      samplerate = 64 * 48000 * (1 << i);
                      if (iasio->canSampleRate(ASIOSampleRate(samplerate)) == ASE_OK)
                        deviceInfo.m_sampleRates.push_back(samplerate);
                    }
                    ASIOChannelInfo info{ 0, ASIOFalse };
	                  if (iasio->getChannelInfo(&info) == ASE_OK)
                      deviceInfo.m_dataFormats.push_back(GetAEDataFormatForASIOSampleType(info.type));
                    deviceInfo.m_dataFormats.push_back(AE_FMT_U8);
                    opt.FormatType = kASIOPCMFormat;
                    iasio->future(kAsioSetIoFormat, &opt);
                  }
                  iasio->Release();
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
		return AE_FMT_U8;
	case ASIOSTDSDInt8MSB1:
		return AE_FMT_U8;
	case ASIOSTDSDInt8NER8:
		return AE_FMT_RAW;
	default:
		return AE_FMT_INVALID;
	}
}

template<typename real_t, typename int_t> int_t ConvertRealToInt(real_t inp_value) {
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

void CAESinkASIO::ConvertSample(void* outPtr, ASIOSampleType outType, void* inpPtr, AEDataFormat inpType)
{
  switch (inpType)
  {
  case AE_FMT_U8:
    break;
  case AE_FMT_FLOAT:
    switch (outType) {
    case ASIOSTInt16MSB:
      {
        auto value = ConvertRealToInt<float, short>(*reinterpret_cast<float*>(inpPtr));
        *reinterpret_cast<short*>(outPtr) = _byteswap_ushort(value);
      }
      break;
    case ASIOSTInt16LSB:
      {
        auto value = ConvertRealToInt<float, short>(*reinterpret_cast<float*>(inpPtr));
        *reinterpret_cast<short*>(outPtr) = value;
      }
      break;
    case ASIOSTInt24MSB:
      {
        auto value = ConvertRealToInt<float, int>(*reinterpret_cast<float*>(inpPtr));
        reinterpret_cast<uint8_t*>(outPtr)[0] = reinterpret_cast<uint8_t*>(&value)[3];
        reinterpret_cast<uint8_t*>(outPtr)[1] = reinterpret_cast<uint8_t*>(&value)[2];
        reinterpret_cast<uint8_t*>(outPtr)[2] = reinterpret_cast<uint8_t*>(&value)[1];
      }
      break;
    case ASIOSTInt24LSB:
      {
        auto value = ConvertRealToInt<float, int>(*reinterpret_cast<float*>(inpPtr));
        reinterpret_cast<uint8_t*>(outPtr)[0] = reinterpret_cast<uint8_t*>(&value)[1];
        reinterpret_cast<uint8_t*>(outPtr)[1] = reinterpret_cast<uint8_t*>(&value)[2];
        reinterpret_cast<uint8_t*>(outPtr)[2] = reinterpret_cast<uint8_t*>(&value)[3];
      }
      break;
    case ASIOSTInt32MSB:
    case ASIOSTInt32MSB16:
    case ASIOSTInt32MSB18:
    case ASIOSTInt32MSB20:
    case ASIOSTInt32MSB24:
      {
        auto value = ConvertRealToInt<float, int>(*reinterpret_cast<float*>(inpPtr));
        *reinterpret_cast<int*>(outPtr) = _byteswap_ulong(value);
      }
      break;
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
      {
        auto value = ConvertRealToInt<float, int>(*reinterpret_cast<float*>(inpPtr));
        *reinterpret_cast<int*>(outPtr) = value;
      }
      break;
    case ASIOSTFloat32MSB:
      {
        auto value = (float)*reinterpret_cast<float*>(inpPtr);
        *reinterpret_cast<uint32_t*>(outPtr) = _byteswap_ulong(*reinterpret_cast<uint32_t*>(&value));
      }
      break;
    case ASIOSTFloat64MSB:
      {
        auto value = (double)*reinterpret_cast<float*>(inpPtr);
        *reinterpret_cast<uint64_t*>(outPtr) = _byteswap_uint64(*reinterpret_cast<uint64_t*>(&value));
      }
      break;
    case ASIOSTFloat32LSB:
      {
        auto value = (float)*reinterpret_cast<float*>(inpPtr);
        *reinterpret_cast<uint32_t*>(outPtr) = *reinterpret_cast<uint32_t*>(&value);
      }
      break;
    case ASIOSTFloat64LSB:
      {
        auto value = (double)*reinterpret_cast<float*>(inpPtr);
        *reinterpret_cast<uint64_t*>(outPtr) = *reinterpret_cast<uint64_t*>(&value);
      }
      break;
    }
    break;
  case AE_FMT_DOUBLE:
    switch (outType) {
    case ASIOSTInt16MSB:
      {
        auto value = ConvertRealToInt<double, short>(*reinterpret_cast<double*>(inpPtr));
        *reinterpret_cast<short*>(outPtr) = _byteswap_ushort(value);
      }
      break;
    case ASIOSTInt16LSB:
      {
        auto value = ConvertRealToInt<double, short>(*reinterpret_cast<double*>(inpPtr));
        *reinterpret_cast<short*>(outPtr) = value;
      }
      break;
    case ASIOSTInt24MSB:
      {
        auto value = ConvertRealToInt<double, int>(*reinterpret_cast<double*>(inpPtr));
        reinterpret_cast<uint8_t*>(outPtr)[0] = reinterpret_cast<uint8_t*>(&value)[3];
        reinterpret_cast<uint8_t*>(outPtr)[1] = reinterpret_cast<uint8_t*>(&value)[2];
        reinterpret_cast<uint8_t*>(outPtr)[2] = reinterpret_cast<uint8_t*>(&value)[1];
      }
      break;
    case ASIOSTInt24LSB:
      {
        auto value = ConvertRealToInt<double, int>(*reinterpret_cast<double*>(inpPtr));
        reinterpret_cast<uint8_t*>(outPtr)[0] = reinterpret_cast<uint8_t*>(&value)[1];
        reinterpret_cast<uint8_t*>(outPtr)[1] = reinterpret_cast<uint8_t*>(&value)[2];
        reinterpret_cast<uint8_t*>(outPtr)[2] = reinterpret_cast<uint8_t*>(&value)[3];
      }
      break;
    case ASIOSTInt32MSB:
    case ASIOSTInt32MSB16:
    case ASIOSTInt32MSB18:
    case ASIOSTInt32MSB20:
    case ASIOSTInt32MSB24:
      {
        auto value = ConvertRealToInt<double, int>(*reinterpret_cast<double*>(inpPtr));
        *reinterpret_cast<int*>(outPtr) = _byteswap_ulong(value);
      }
      break;
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
      {
        auto value = ConvertRealToInt<double, int>(*reinterpret_cast<double*>(inpPtr));
        *reinterpret_cast<int*>(outPtr) = value;
      }
      break;
    case ASIOSTFloat32MSB:
      {
        auto value = (float)*reinterpret_cast<double*>(inpPtr);
        *reinterpret_cast<uint32_t*>(outPtr) = _byteswap_ulong(*reinterpret_cast<uint32_t*>(&value));
      }
      break;
    case ASIOSTFloat64MSB:
      {
        auto value = (double)*reinterpret_cast<double*>(inpPtr);
        *reinterpret_cast<uint64_t*>(outPtr) = _byteswap_uint64(*reinterpret_cast<uint64_t*>(&value));
      }
      break;
    case ASIOSTFloat32LSB:
      {
        auto value = (float)*reinterpret_cast<double*>(inpPtr);
        *reinterpret_cast<uint32_t*>(outPtr) = *reinterpret_cast<uint32_t*>(&value);
      }
      break;
    case ASIOSTFloat64LSB:
      {
        auto value = (double)*reinterpret_cast<double*>(inpPtr);
        *reinterpret_cast<uint64_t*>(outPtr) = *reinterpret_cast<uint64_t*>(&value);
      }
      break;
    }
    break;
  }
}

void CAESinkASIO::ConvertSamples(void* outPtr, ASIOSampleType outType, unsigned int outSampleSize, void* inpPtr, unsigned int channel, unsigned int channels, AEDataFormat inpType, unsigned int inpSampleSize, unsigned int samples)
{
  for (auto sample = 0U; sample < samples; sample++)
  {
    auto inpSamplePtr = (uint8_t*)inpPtr + (sample * channels + channel) * inpSampleSize;
    auto outSamplePtr = (uint8_t*)outPtr + sample * outSampleSize;
    ConvertSample(outSamplePtr, outType, inpSamplePtr, inpType);
  }
}

void CAESinkASIO::ZeroSamples(void* outPtr, ASIOSampleType outType, unsigned int outSampleSize, unsigned int samples)
{
  switch (outType)
  {
  case ASIOSTDSDInt8MSB1:
  case ASIOSTDSDInt8LSB1:
    std::memset(outPtr, DSD_SILENCE_BYTE, samples);
    break;
  case ASIOSTDSDInt8NER8:
    for (auto sample = 0U; sample < samples; sample++)
      ((uint8_t*)outPtr)[sample] = (DSD_SILENCE_BYTE >> (7 - sample % 8)) & 1;
    break;
  default:
    std::memset(outPtr, 0, samples * outSampleSize);
    break;
  }
}

IASIO* CAESinkASIO::Load(std::string& device)
{
  IASIO* iasio{ nullptr };
  CLSID clsId;
  if (SUCCEEDED(CLSIDFromString((LPOLESTR)KODI::PLATFORM::WINDOWS::ToW(device).c_str(), &clsId)))
  {
    if (SUCCEEDED(CoCreateInstance(clsId, nullptr, CLSCTX_INPROC_SERVER, clsId, (LPVOID*)&iasio)))
    {
      if (iasio->init(nullptr) != ASIOTrue)
      {
        iasio->Release();
        iasio = nullptr;
      }
    }
  }
  return iasio;
}

// ASIO callbacks

void CAESinkASIO::s_bufferSwitch(long doubleBufferIndex, ASIOBool directProcess)
{
  s_this->bufferSwitch(doubleBufferIndex, directProcess);
}

void CAESinkASIO::s_sampleRateDidChange(ASIOSampleRate sampleRate)
{
  s_this->sampleRateDidChange(sampleRate);
}

long CAESinkASIO::s_asioMessage(long selector, long value, void* message, double* opt)
{
  return s_this->asioMessage(selector, value, message, opt);
}

ASIOTime* CAESinkASIO::s_bufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess)
{
  static ASIOTime t{};
  s_this->bufferSwitch(doubleBufferIndex, directProcess);
  return &t;
}

void CAESinkASIO::bufferSwitch(long doubleBufferIndex, ASIOBool directProcess)
{
  if (m_bufferSize * m_sampleSize <= m_planeBuffer.GetReadSize())
  {
    for (auto plane = 0; plane < m_planeCount; plane++)
    {
      m_planeBuffer.Read((uint8_t*)m_bufferInfos[plane].buffers[doubleBufferIndex], m_bufferSize * m_sampleSize, plane);
    }
  }
  else
  {
    for (auto plane = 0; plane < m_planeCount; plane++)
    {
      ZeroSamples(m_bufferInfos[plane].buffers[doubleBufferIndex], m_sampleType, m_sampleSize, m_bufferSize);
    }
  }
}

void CAESinkASIO::sampleRateDidChange(ASIOSampleRate sRate)
{
}

long CAESinkASIO::asioMessage(long selector, long value, void* message, double* opt)
{
  return 0L;
}
