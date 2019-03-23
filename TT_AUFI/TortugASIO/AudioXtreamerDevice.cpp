#include "stdafx.h"
#include "AudioXtreamerDevice.h"
#include <process.h>


AudioXtreamerDevice::AudioXtreamerDevice(UsbDeviceClient & client, ASIOSettings::Settings & params)
  : UsbDevice(client, params)
  , hMapFile(NULL)
  , hASIOMutex(NULL)
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
  hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, szNameShMem);
  if (hMapFile == NULL) {
    _tprintf(TEXT("OpenFileMapping failed (%d).\n"), GetLastError());
    goto error;
  }

  hWnd = FindWindow(szNameClass, szNameApp);
  if (hWnd == NULL) {
    _tprintf(TEXT("Xtreamer window not found (%d).\n"), GetLastError());
    goto error;
  } else {
    if ( SendMessage(hWnd, WM_XTREAMER, 1, 0) == 0 )
      goto error;
  }

  pStreamParams = (uint8_t*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, (1 << 16));
  pRxBuf = (uint8_t*)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, (1 << 16), (1 << 16));
  pTxBuf = (uint8_t*)MapViewOfFile(hMapFile, FILE_MAP_WRITE, 0, (2 << 16) , (1 << 16));

  if (pStreamParams == nullptr || pTxBuf == nullptr || pTxBuf == nullptr)
    _tprintf(TEXT("Could not map view of file (%d).\n"), GetLastError());
  else
    return true;

error:

  unmap(pTxBuf);
  unmap(pRxBuf);
  unmap(pStreamParams);

  unhandle(hASIOMutex);
  unhandle(hMapFile);
  return false;
}


bool
AudioXtreamerDevice::Start()
{
  LOG0("AudioXtreamerDevice::Start");

  hASIOMutex = OpenMutex(SYNCHRONIZE, FALSE, szNameMutex);

  if (hASIOMutex == NULL || SendMessage(hWnd, WM_XTREAMER, 2, 0) == 0)
    return false;


  if (hth_Worker != INVALID_HANDLE_VALUE) {
    DWORD dwExCode;
    GetExitCodeThread(hth_Worker, &dwExCode);
    if (dwExCode == STILL_ACTIVE)
      return true;
  }

  hth_Worker = (HANDLE)_beginthread(StaticWorkerThread, 0, this);
  if (hth_Worker != INVALID_HANDLE_VALUE) {
    ::SetThreadPriority(hth_Worker, THREAD_PRIORITY_TIME_CRITICAL);
    return true;
  }
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
      wait = false;
    }

    if (wait) {
      WaitForSingleObject(hth_Worker, INFINITE);
      hth_Worker = INVALID_HANDLE_VALUE;
    }

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

  unhandle(hASIOMutex);
  unhandle(hMapFile);
  return true;
}


bool AudioXtreamerDevice::GetStatus(UsbDeviceStatus &status)
{
  return false;
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
  //once started, wait until we acquire the mutex, meaning there is data to be sent to switch of asio

  while (WaitForSingleObject(mExitHandle, 0) != WAIT_OBJECT_0)
  {

    DWORD result = WaitForSingleObject(hASIOMutex, 1000);

    switch (result)
    {
    case WAIT_OBJECT_0: {
        ASIOSettings::StreamInfo *info = (ASIOSettings::StreamInfo *)pStreamParams;
        devClient.Switch(info->RxStride, pRxBuf + info->RxOffset, info->TxStride, pTxBuf + info->TxOffset);

        info->Flags |= 0x1; //im alive
        ReleaseMutex(hASIOMutex);

    } break;

    case WAIT_TIMEOUT: // the driver is not present so there is probably some config going on, it should be abandonend soon for a restart.
      break;
    case WAIT_ABANDONED: //now I am the owner of an orphan, release it and leave
      ReleaseMutex(hASIOMutex);
    case WAIT_FAILED:
      error = true;
      break;
    }
    if (error)
      break;
  }

  unhandle(hASIOMutex);

  CloseHandle(mExitHandle);
  mExitHandle = INVALID_HANDLE_VALUE;

  devClient.DeviceStopped(error);

  LOG0("AudioXtreamerDevice::main Exit");
}
