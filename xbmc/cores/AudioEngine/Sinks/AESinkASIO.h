/*
 *  Copyright (C) 2025 Maxim V.Anisiutkin maxim.anisiutkin@gmail.com
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/AudioEngine/Interfaces/AESink.h"
#include "cores/AudioEngine/Utils/AEDeviceInfo.h"
#include "cores/AudioEngine/Utils/AERingBuffer.h"
#include "threads/CriticalSection.h"

#include <stdint.h>

#include <mmsystem.h> /* Microsoft can't write standalone headers */
#include <DSound.h> /* Microsoft can't write standalone headers */
#include <wrl/client.h>

#include <iasiodrv.h>
#undef WINDOWS // Undefine what ASIO just defined

class CAESinkASIO : public IAESink
{
  static constexpr unsigned int  DSD_MIN_SAMPLERATE{ 2822400 };
  static constexpr unsigned char DSD_SILENCE_BYTE{ 0x69 };

public:
  virtual const char *GetName() { return "ASIO"; }

  CAESinkASIO();
  virtual ~CAESinkASIO();

  static void Register();
  static std::unique_ptr<IAESink> Create(std::string& device, AEAudioFormat& desiredFormat);

  virtual bool Initialize(AEAudioFormat &format, std::string &device);
  virtual void Deinitialize();

  virtual void Stop();
  virtual void Drain();
  virtual void GetDelay(AEDelayStatus& status);
  virtual double GetCacheTotal();
  virtual unsigned int AddPackets(uint8_t **data, unsigned int frames, unsigned int offset);

  static void EnumerateDevicesEx (AEDeviceInfoList &deviceInfoList, bool force = false);
private:
  inline static unsigned int GetASIOSampleSizeInBits(ASIOSampleType sampleType);
  inline static AEDataFormat GetAEDataFormatForASIOSampleType(ASIOSampleType sampleType);
  inline static void ConvertSample(void* outPtr, ASIOSampleType outType, void* inpPtr, AEDataFormat inpType);
  inline static void ConvertSamples(void* outPtr, ASIOSampleType outType, unsigned int outSampleSize, void* inpPtr, unsigned int channel, unsigned int channels, AEDataFormat inpType, unsigned int inpSampleSize, unsigned int samples);
  inline static void ZeroSamples(void* outPtr, ASIOSampleType outType, unsigned int outSampleSize, unsigned int samples);
  static IASIO* Load(std::string& device);

  // ASIO callbacks
	static void s_bufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
	static void s_sampleRateDidChange(ASIOSampleRate sampleRate);
	static long s_asioMessage(long selector, long value, void* message, double* opt);
	static ASIOTime* s_bufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);

  void bufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
	void sampleRateDidChange(ASIOSampleRate sampleRate);
	long asioMessage(long selector, long value, void* message, double* opt);

  static CAESinkASIO*         s_this;
  IASIO*                      m_iasio;
  AEAudioFormat               m_format;
  unsigned int                m_planeCount;
  AERingBuffer                m_planeBuffer;
  std::vector<uint8_t>        m_planePad;
  ASIOSampleType              m_sampleType;
  unsigned int                m_sampleSize;
  unsigned int                m_bufferSize;
  std::vector<ASIOBufferInfo> m_bufferInfos;
  unsigned int                m_frameSize;
  unsigned int                m_frameCount;
  unsigned int                m_planeBytesPerSec;
  bool                        m_initialized;
  bool                        m_running;
};
