#include "stdafx.h"
#include "AudioXtreamerDevice.h"
#include "AudioXtreamer\AppLog.h"
#include <process.h>

#include "avrt.h"
#pragma comment(lib, "avrt.lib")


AudioXtreamerDevice::AudioXtreamerDevice(UsbDeviceClient & client)
  : UsbDevice(client)
  , hMapFile(NULL)
  , hAsioEvent(NULL)
  , hExtreamerEvent(NULL)
  , hWnd(NULL)
  , pStreamParams(nullptr)
  , pRxBuf(nullptr)
  , pTxBuf(nullptr)
{
}


AudioXtreamerDevice::~AudioXtreamerDevice()
{
}

inline void unmap(uint8_t* & ptr)
{
  if (ptr) {
    UnmapViewOfFile(ptr);
    ptr = nullptr;
  }
}

inline void unhandle(HANDLE & h)
{
  if (h) {
    CloseHandle(h);
    h = NULL;
  }
}


bool
AudioXtreamerDevice::Open()
{
  AppLog::Info("ASIO IPC", "Opening connection to the AudioXtreamer application");
  hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, szNameShMem);
  if (hMapFile == NULL) {
    AppLog::Error("ASIO IPC", "Shared memory is unavailable (Windows error %lu)", GetLastError());
    _tprintf(TEXT("OpenFileMapping failed (%d).\n"), GetLastError());
    goto error;
  }

  hWnd = FindWindow(szNameClass, szNameApp);
  if (hWnd == NULL) {
    AppLog::Error("ASIO IPC", "AudioXtreamer application window was not found");
    _tprintf(TEXT("Xtreamer window not found (%d).\n"), GetLastError());
    goto error;
  } else {
    if ( SendMessage(hWnd, WM_XTREAMER, 1, 0) == 0 ) {
      AppLog::Error("ASIO IPC", "AudioXtreamer reports that no USB device is ready");
      goto error;
    }
  }

  pStreamParams = (uint8_t*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, (1 << SH_MEM_BLK_SIZE_SHIFT));
  pRxBuf = (uint8_t*)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, (1 << SH_MEM_BLK_SIZE_SHIFT), (1 << SH_MEM_BLK_SIZE_SHIFT));
  pTxBuf = (uint8_t*)MapViewOfFile(hMapFile, FILE_MAP_WRITE, 0, (2 << SH_MEM_BLK_SIZE_SHIFT) , 0); //till the end

  if ( (pStreamParams == nullptr) || (pRxBuf == nullptr) || (pTxBuf == nullptr) ) {
    AppLog::Error("ASIO IPC", "Could not map the shared audio buffers (Windows error %lu)", GetLastError());
    _tprintf(TEXT("Could not map view of file (%d).\n"), GetLastError());
  }
  else {
    AppLog::Info("ASIO IPC", "Connection to AudioXtreamer established");
    return true;
  }

error:

  unmap(pTxBuf);
  unmap(pRxBuf);
  unmap(pStreamParams);

  unhandle(hAsioEvent);
  unhandle(hMapFile);
  return false;
}

ASIOSettings::StreamInfo AudioXtreamerDevice::GetStreamInfo()
{
  ASIOSettings::StreamInfo s;
  ZeroMemory(&s, sizeof(s));
  if (pStreamParams == nullptr)
    return s;
  else
    return *(ASIOSettings::StreamInfo*)pStreamParams;
}

bool
AudioXtreamerDevice::Start()
{
  LOG0("AudioXtreamerDevice::Start");
  
  hAsioEvent = OpenEvent(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, szNameAsioEvent);
  hExtreamerEvent = OpenEvent(SYNCHRONIZE, FALSE, szNameXtreamerEvent);

  if (hAsioEvent == NULL || hExtreamerEvent == NULL || SendMessage(hWnd, WM_XTREAMER, 2, 0) == 0) {
    AppLog::Error("ASIO IPC", "Could not start streaming; IPC events or USB engine are unavailable");
    return false;
  }


  if (hth_Worker != INVALID_HANDLE_VALUE) {
    DWORD dwExCode;
    GetExitCodeThread(hth_Worker, &dwExCode);
    if (dwExCode == STILL_ACTIVE)
      return true;
  }

  hth_Worker = (HANDLE)_beginthread(StaticWorkerThread, 0, this);
  if (hth_Worker != INVALID_HANDLE_VALUE) {
    ::SetThreadPriority(hth_Worker, THREAD_PRIORITY_TIME_CRITICAL);
    AppLog::Info("ASIO IPC", "ASIO bridge worker started");
    return true;
  }
  AppLog::Error("ASIO IPC", "Could not create the ASIO bridge worker");
  return false;
}


bool
AudioXtreamerDevice::Stop(bool wait)
{
  LOG0("AudioXtreamerDevice::Stop");

  if (mExitHandle != INVALID_HANDLE_VALUE) {
    BOOL result = SetEvent(mExitHandle);
    if (result == 0) {
      LOGN("AudioXtreamerDevice::Stop SetEvent HANDLE:%p Error %u\n", mExitHandle, GetLastError() );
      AppLog::Error("ASIO IPC", "Could not signal the ASIO bridge to stop (Windows error %lu)", GetLastError());
      wait = false;
    }

    if (wait) {
      WaitForSingleObject(hth_Worker, INFINITE);
      hth_Worker = INVALID_HANDLE_VALUE;
    }

    AppLog::Info("ASIO IPC", "ASIO bridge stop requested%s", wait ? " and completed" : "");
    return true;
  }
  else
    return false;
}


bool
AudioXtreamerDevice::Close()
{
  LOG0("AudioXtreamerDevice::Close");
  Stop(true);
  unmap(pTxBuf);
  unmap(pRxBuf);
  unmap(pStreamParams);

  unhandle(hAsioEvent);
  unhandle(hExtreamerEvent);
  unhandle(hMapFile);
  AppLog::Info("ASIO IPC", "Connection to AudioXtreamer closed");
  return true;
}


bool AudioXtreamerDevice::GetStatus(UsbDeviceStatus &status)
{
  return false;
}

uint32_t AudioXtreamerDevice::GetSampleRate()
{
  return uint32_t( SendMessage(hWnd, WM_XTREAMER, 4, 0));
}

bool AudioXtreamerDevice::ConfigureDevice()
{
  return SendMessage(hWnd, WM_XTREAMER, 3, 0) == 1;
}


bool
AudioXtreamerDevice::IsRunning()
{
  return true;
}


bool
AudioXtreamerDevice::IsPresent()
{
  return true;
}

void
AudioXtreamerDevice::main()
{
  LOG0("AudioXtreamerDevice::main");
  bool error = false;
  mExitHandle = CreateEvent(NULL, TRUE, FALSE, NULL);
  ResetEvent(mExitHandle);
  //once started, fire the event to tell audioextreamer we are alive

  volatile ASIOSettings::StreamInfo* info = (ASIOSettings::StreamInfo*)pStreamParams;

  DWORD proAudioIndex = 0;
  HANDLE AvrtHandle = AvSetMmThreadCharacteristics(L"Pro Audio", &proAudioIndex);
  AvSetMmThreadPriority(AvrtHandle, AVRT_PRIORITY_CRITICAL);


  while (WaitForSingleObject(mExitHandle, 0) != WAIT_OBJECT_0)
  {
    info->Flags |= 0x1; //im alive
    SetEvent(hAsioEvent);
    DWORD result = WaitForSingleObject(hExtreamerEvent, 1000);
    switch (result)
    {
    case WAIT_OBJECT_0: {

        if (info->Flags & 0x2)
          devClient.SampleRateChanged();
        else
          devClient.Switch(0, info->RxStride, pRxBuf + info->RxOffset, info->TxStride, pTxBuf + info->TxOffset);

    } break;

    case WAIT_TIMEOUT: // the driver is not present so there is probably some config going on, it should be abandonend soon for a restart.
      break;

    case WAIT_ABANDONED:

    case WAIT_FAILED:
      AppLog::Error("ASIO IPC", "ASIO bridge wait failed (Windows error %lu)", GetLastError());
      error = true;
      break;
    }
    if (error)
      break;
  }

  info->Flags &= ~((uint32_t)0x1);//Im gone
  SetEvent(hAsioEvent);

  unhandle(hAsioEvent);
  unhandle(hExtreamerEvent);

  AvRevertMmThreadCharacteristics(AvrtHandle);

  CloseHandle(mExitHandle);
  mExitHandle = INVALID_HANDLE_VALUE;

  devClient.DeviceStopped(error);

  AppLog::Info("ASIO IPC", "ASIO bridge worker exited%s", error ? " after an error" : " normally");

  LOG0("AudioXtreamerDevice::main Exit");
}
