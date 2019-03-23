#pragma once

#include "UsbDev\UsbDev.h"

class CypressDevice : public UsbDevice
{
public:
  explicit CypressDevice(UsbDeviceClient & client, ASIOSettings::Settings & params);
  ~CypressDevice();

  bool Open() override;
  bool Start() override;
  bool Stop(bool wait) override;
  bool Close() override;
  bool IsRunning() override;
  bool IsPresent() override;

  bool GetStatus(UsbDeviceStatus &status) override;
  bool ConfigureDevice() override { return false; }

private:

  void main();
  volatile HANDLE hth_Worker;
  volatile HANDLE mExitHandle;

  static void StaticWorkerThread(void* arg)
  {
    CypressDevice *inst = static_cast<CypressDevice*>(arg);
    if (inst != nullptr)
      inst->main();
  }

  uint8_t mDefOutEP;
  uint8_t mDefInEP;
  uint8_t* mBitstream;
  uint32_t mRsize;

  HANDLE mDevHandle;
  HANDLE hSem;
  UsbDeviceStatus mDevStatus;
};
