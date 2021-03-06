#pragma once
#include <stdint.h>
#include "AudioXtreamer\ASIOSettings.h"

typedef struct _UsbDeviceStatus
{
  uint32_t ResyncErrors;
  uint32_t FifoLevel;
  uint32_t OutSkipCount;
  uint32_t InFullCount;
  uint32_t LastSR;
  uint32_t SwSR;
  uint32_t Ep6IsoErr;

} UsbDeviceStatus;

class UsbDeviceClient
{
public:
  virtual bool Switch(uint32_t timeout, uint32_t rxSampleSize, uint8_t *rxBuff, uint32_t txSampleSize, uint8_t *txBuff) = 0;
  virtual void AllocBuffers(uint32_t rxSize, uint8_t *&rxBuff, uint32_t txSize, uint8_t *&txBuff) = 0;
  virtual void FreeBuffers(uint8_t *&rxBuff, uint8_t *&txBuff) = 0;
  virtual void SampleRateChanged() = 0;
  virtual void DeviceStopped(bool error) = 0;
  virtual HANDLE GetSwitchHandle() { return NULL; };
  virtual bool ClientPresent() { return true; }
};


class UsbDevice
{
public:
  virtual ~UsbDevice() = 0;
  virtual bool Open() = 0;
  virtual bool Close() = 0;
  virtual bool Start() = 0;
  virtual bool Stop(bool wait) = 0;
  virtual bool IsRunning() = 0;
  virtual bool IsPresent() = 0;
  virtual bool GetStatus(UsbDeviceStatus &status) = 0;
  virtual uint32_t GetSampleRate() = 0;
  virtual bool ConfigureDevice() = 0;

protected:
  UsbDevice(UsbDeviceClient & client, ASIOSettings::Settings & params)
    : devClient(client), devParams(params) {}
  UsbDeviceClient & devClient;
  ASIOSettings::Settings & devParams;
};
