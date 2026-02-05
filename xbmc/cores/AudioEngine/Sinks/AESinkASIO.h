/*
 *  Copyright (C) 2026 Maxim V.Anisiutkin maxim.anisiutkin@gmail.com
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/AudioEngine/Interfaces/AESink.h"
#include "cores/AudioEngine/Utils/AEDeviceInfo.h"
#include "cores/AudioEngine/Utils/AERingBuffer.h"
#include <stdint.h>
#include <condition_variable>
#include <mutex>

#include <iasiodrv.h>
#undef WINDOWS // Undefine what ASIO has just defined

enum class ASIOState
{
  UNLOADED,
  LOADED,
  INITIALIZED,
  PREPARED,
  RUNNING
};

class CAESinkASIO : public IAESink
{
  static constexpr unsigned int ASIO_MAX_CHANNEL_COUNT{8U};
  static constexpr double ASIO_CACHED_SECONDS{1.0};
  static constexpr unsigned char DSD_SILENCE_BYTE{0x69};
  static constexpr uint8_t REVERSE_BIT_TABLE[256]
  {
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0,
    0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
    0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8,
    0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
    0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4,
    0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC,
    0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
    0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2,
    0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA,
    0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6,
    0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
    0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE,
    0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
    0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1,
    0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
    0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9,
    0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
    0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5,
    0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
    0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED,
    0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
    0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3,
    0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
    0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB,
    0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
    0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7,
    0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
    0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF,
    0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF
  };

public:
  const char *GetName() override { return "ASIO"; }

  CAESinkASIO();
  virtual ~CAESinkASIO() override;

  static void Register();
  static std::unique_ptr<IAESink> Create(std::string& device, AEAudioFormat& desiredFormat);
  static void EnumerateDevicesEx(AEDeviceInfoList& deviceInfoList, bool force = false);
  static void Cleanup();

  virtual bool Initialize(AEAudioFormat& format, std::string& device) override;
  virtual void Deinitialize() override;
  virtual double GetCacheTotal() override;
  virtual double GetLatency() override;
  virtual unsigned int AddPackets(uint8_t** data, unsigned int frames, unsigned int offset) override;
  virtual void AddPause(unsigned int millis) override;
  virtual void GetDelay(AEDelayStatus& status) override;
  virtual void Drain() override;
  virtual bool HasVolume() override;
  virtual void SetVolume(float volume) override;

private:
  bool CreateBuffers();
  bool DisposeBuffers();
  bool Start();
  bool Stop();

  inline static unsigned int GetASIOSampleSizeInBits(ASIOSampleType sampleType);
  inline static AEDataFormat GetAEDataFormatForASIOSampleType(ASIOSampleType sampleType);
  inline static void ConvertSample(void* outValue, ASIOSampleType outType, const void* inpValue, AEDataFormat inpType);
  inline static void ZeroSamples(void* data, ASIOSampleType type, unsigned int samples);
  static IASIO* LoadDriver(std::string& device);
  static bool IsNativeDSDDevice(IASIO* driver);

  // ASIO callbacks
	static void s_bufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
	static void s_sampleRateDidChange(ASIOSampleRate sampleRate);
	static long s_asioMessage(long selector, long value, void* message, double* opt);
	static ASIOTime* s_bufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);

  void bufferSwitch(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
	void sampleRateDidChange(ASIOSampleRate sampleRate);
	long asioMessage(long selector, long value, void* message, double* opt);

  static CAESinkASIO*         ms_this;
  std::mutex                  m_mutex;
  std::condition_variable     m_cv;
  AEAudioFormat               m_format;
  std::string                 m_device;
  IASIO*                      m_driver;
  ASIOState                   m_state;
  bool                        m_nativeDSD;
  bool                        m_outputDSD;
  std::vector<uint8_t>        m_plane;
  AERingBuffer                m_cache;
  unsigned int                m_channels;
  ASIOSampleRate              m_sampleRate;
  ASIOSampleType              m_sampleType;
  long                        m_sampleBits;
  long                        m_bufferSize;
  std::vector<ASIOBufferInfo> m_bufferInfos;
  unsigned int                m_sampleSize;
  unsigned int                m_accessSize;
  unsigned int                m_frameSize;
  unsigned int                m_frameCount;
  unsigned int                m_planeBytesPerSec;
  bool                        m_initialized;
};
