﻿#include "stdafx.h"

#include <process.h>
#include <stdio.h>


#include "CypressDevice.h"

#include "UsbBackend.h"

#include "ZTEXDev\ztexdev.h"
#include "resource.h"


#include <tchar.h>


#pragma comment(lib, "Setupapi.lib")
#pragma comment(lib, "Rpcrt4.lib")

#include "avrt.h"
#pragma comment(lib, "avrt.lib")


int gcd(int a, int b)
{
  int r; // remainder
  while (b > 0) {
    r = a % b;
    a = b;
    b = r;
  }
  return a;
}

#define LCM(a,b) (((a)*(b))/gcd(a,b))

using namespace ASIOSettings;

uint8_t setyb[256];

CypressDevice::CypressDevice(UsbDeviceClient & client, ASIOSettings::Settings &params )
  : UsbDevice(client,params)
  , asioInPtr(nullptr)
  , asioOutPtr(nullptr)
  , mDevStatus({ 0 })
  , mFileHandle(NULL)
  , mASIOHandle(NULL)
  , mTxRequests(nullptr)
  , mRxRequests(nullptr)
{
  LOG0("CypressDevice::CypressDevice");

  mDefOutEP = 0;
  mDefInEP = 0;
  mDevHandle = INVALID_HANDLE_VALUE;
  hth_Worker = INVALID_HANDLE_VALUE;
  mExitHandle = INVALID_HANDLE_VALUE;


  hSem = CreateSemaphore(
    NULL,           // default security attributes
    1,  // initial count
    1,  // maximum count
    NULL);

  for (int b = 0; b < 256; ++b)
  {
    setyb [b] = ((b & 128) >> 7) |
      ((b & 64) >> 5) |
      ((b & 32) >> 3) |
      ((b & 16) >> 1) |
      ((b & 8) << 1) |
      ((b & 4) << 3) |
      ((b & 2) << 5) |
      ((b & 1) << 7);
  }

  HRSRC hrc = FindResource(NULL, MAKEINTRESOURCE(IDR_FPGA_BIN), _T("RC_DATA"));
  HGLOBAL hg = LoadResource(NULL, hrc);
  uint8_t* bits = (uint8_t*)LockResource(hg);
  mBitstream = nullptr;

  if (bits)
  {
    mResourceSize = SizeofResource(NULL, hrc);
    
    uint8_t * bitstream = (uint8_t *)malloc(mResourceSize +512);
    ZeroMemory(bitstream, 512);

    uint8_t* buf = bitstream + 512;
    for (uint32_t c = 0; c < mResourceSize; c++, buf++) {
      register uint8_t b = bits[c];
      *buf = setyb[b];
    }

    mBitstream = bitstream;
    mResourceSize += 512;
  }
}

CypressDevice::~CypressDevice()
{
  LOG0("CypressDevice::~CypressDevice");

  if (mBitstream) {
    free(mBitstream);
    mBitstream = nullptr;
  }

  if (mExitHandle != INVALID_HANDLE_VALUE)
    DebugBreak();


  CloseHandle(hSem);
}

bool CypressDevice::Start()
{
  LOG0("CypressDevice::Start");
  if (mDevHandle == INVALID_HANDLE_VALUE)
    return false;
  //let's try to identify the fpga
  int64_t get_result = ztex_default_lsi_get1(mDevHandle, 0);
  static const char* trtg = "TRTG";
  if (get_result != *(uint32_t*)trtg )
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

bool CypressDevice::Stop(bool wait)
{
  LOG0("CypressDevice::Stop");

  
  if (mExitHandle != INVALID_HANDLE_VALUE) {
    BOOL result = SetEvent(mExitHandle);
    if (result == 0) {
      LOG0("CypressDevice::Stop SetEvent Exit FAILED!");
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

//---------------------------------------------------------------------------------------------

bool CypressDevice::Open()
{
  LOG0("CypressDevice::Open");
  HANDLE handle = INVALID_HANDLE_VALUE;
  ztex_device_info info;
  mDefOutEP = 0;
  mDefInEP = 0;
  memset(&info, 0, sizeof(ztex_device_info));

  if (!bknd_open(handle, mFileHandle))
    goto err;

  int status = ztex_get_device_info(handle, &info);
  if (status < 0) {
    fprintf(stderr, "Error: Unable to get device info\n");
    goto err;
  }

  mDefInEP = info.default_in_ep;
  mDefOutEP = info.default_out_ep;

  //status = ztex_get_fpga_config(handle);
  //if (status == -1)
  //  goto err;
  //else if (status == 0)
  {

   if (mBitstream != nullptr) {
#define EP0_TRANSACTION_SIZE 2048

      // reset FPGA
      status = (BOOL)control_transfer(handle, 0x40, 0x31, 0, 0, NULL, 0, 1500);
      // transfer data
      status = 0;
      uint32_t last_idx = mResourceSize % EP0_TRANSACTION_SIZE;
      int bufs_idx = (mResourceSize / EP0_TRANSACTION_SIZE);

      for (int i = 0; (status >= 0) && (i < bufs_idx); i++)
        status =(BOOL)control_transfer(handle, 0x40, 0x32, 0, 0, mBitstream + (i * EP0_TRANSACTION_SIZE), EP0_TRANSACTION_SIZE, 1500);

      if (last_idx)
        status =(BOOL)control_transfer(handle, 0x40, 0x32, 0, 0, mBitstream + bufs_idx * EP0_TRANSACTION_SIZE, last_idx, 1500);

    } else {
     goto err;
    }

    fflush(stderr);
    // check config
    status = ztex_get_fpga_config(handle);
    if (status < 0) {
      fprintf(stderr, "Error: Unable to get FPGA configuration state\n");
      goto err;
    }
    else if (status == 0) {
      fprintf(stderr, "Error: FPGA not configured\n");
      goto err;
    }
  }

  ztex_default_reset(handle, 0);

  status = 0;
  goto noerr;

err:
  status = 1;
  if (handle != INVALID_HANDLE_VALUE)
    bknd_close(handle, mFileHandle);

noerr:

  mDevHandle = status == 0 ? handle : INVALID_HANDLE_VALUE;
  if (status == 0)
    return true;
  else
    return false;
}

//---------------------------------------------------------------------------------------------
bool CypressDevice::Close()
{
  LOG0("CypressDevice::Close");
  //Check Thread Handle
  DWORD dwExCode;

  if (hth_Worker != INVALID_HANDLE_VALUE) {
    GetExitCodeThread(hth_Worker, &dwExCode);
    if (dwExCode == STILL_ACTIVE)
      Stop(true);
  }

  if (mDevHandle != INVALID_HANDLE_VALUE) {

    bknd_close(mDevHandle, mFileHandle);
    //wait for disconnection before a reattempt to open
    Sleep(10);
    mDevHandle = INVALID_HANDLE_VALUE;
  }

  return true;
}

//---------------------------------------------------------------------------------------------


/*Register Description accessed through the lsi 256 32bit regs
  0x00 COOKIE must read allways "TRTG"
  0x01 VERSION NR
  0x02 Detected word clock refs(16)/cycles(16) with 48mhz as ref so
       a perfect 48k sampling rate should count 0x0000BB80
  0x03 Debug register 
  0x04  b3: padding | b2: fifo depth | b1: nr_samples | b0(ins):ins  | b0(4):outs
  0x05 header filling(16bit)

  0x08 i/o matrix mapping
*/



bool CypressDevice::IsRunning() {
  return mExitHandle != INVALID_HANDLE_VALUE;
}

bool CypressDevice::IsPresent() {
  return mDevHandle != INVALID_HANDLE_VALUE;
}

#define LAP(start,stop,freq) ((uint32_t)(((stop.QuadPart - start.QuadPart) * 1000000) / freq.QuadPart))
template<typename T1, typename T2>
constexpr auto NrPackets(T1 size, T2 len) { return ( (size / len) + (size % len ? 1 : 0) ); }

#define SNAP_TOLERANCE 100
#define SNAP_TO_AND_RET(val,snapto) { if(val > (snapto - SNAP_TOLERANCE) && val < (snapto + SNAP_TOLERANCE)) return snapto; }

constexpr auto ConvertSampleRate(uint32_t srReg)
{
  uint16_t count = (uint16_t)(srReg & 0xFFFF);
  uint16_t fract = (uint16_t)(srReg >> 16);
  uint32_t sr = count * 10;
  //mSRacc -= mSRvals[mSRidx];
  //mSRacc += sr;
  //mSRvals[mSRidx] = sr;
  //mSRidx = (mSRidx + 1) & 0xf;

  SNAP_TO_AND_RET(sr, 44100);
  SNAP_TO_AND_RET(sr, 48000);
  SNAP_TO_AND_RET(sr, 88200);
  SNAP_TO_AND_RET(sr, 96000);

  return 0;
}

static const uint16_t Sine48[48] =
{ 32768, 37045, 41248, 45307, 49151, 52715, 55938, 58764,
61145, 63041, 64418, 65255, 65535, 65255, 64418, 63041,
61145, 58764, 55938, 52715, 49151, 45307, 41248, 37045,
32768, 28490, 24287, 20228, 16384, 12820, 9597, 6771,
4390, 2494, 1117, 280, 0, 280, 1117, 2494,
4390, 6771, 9597, 12820, 16384, 20228, 24287, 28490 };

#pragma pack (push,1)
struct RxHeader
{
  uint16_t Hdr1;
  uint16_t Hdr2;
  uint16_t SamplingRate;
  uint16_t FifoLevel;
  uint16_t OutSkipCount;
  uint16_t InFullCount;
  uint8_t midi_in[8];
};
#pragma pack (pop)

static const uint32_t scTxHeaderSize = 4;

bool CypressDevice::ProcessHdr(uint8_t* pHdr)
{
  struct RxHeader* hdr = (struct RxHeader* )pHdr;

  if (hdr->Hdr1 == 0xaaaa && hdr->Hdr2 == 0x5555)
  {

    uint32_t SR = ConvertSampleRate(hdr->SamplingRate);
    if (mDevStatus.LastSR != SR && mDevStatus.LastSR != -1 && mDevStatus.LastSR != 0)
    {
      devClient.SampleRateChanged();
    }
    mDevStatus.LastSR = SR;

    mDevStatus.FifoLevel    = hdr->FifoLevel;
    mDevStatus.OutSkipCount = hdr->OutSkipCount;
    mDevStatus.InFullCount  = hdr->InFullCount;

    midi.MidiIn(hdr->midi_in);
    return true;
  }
  return false;
};

//initialize header mark
void CypressDevice::InitTxHeaders(uint8_t* ptr, uint32_t Samples)
{
  USHORT w = 0;
  ZeroMemory(ptr, Samples * OUTStride);
  for (uint32_t i = 0; i < Samples; i++)
  {
    PUCHAR p = ptr + i * OUTStride;
    *p = 0xAA;
    *(p + 1) = 0x55;
    *(p + 2) = 0x55;
    *(p + 3) = 0xAA;
#if LOOPBACK_TEST
    /*for (uint32_t c = 0; c < (nrOuts/2)*3; ++c)
    {
      *(PUSHORT)(ptr + i * OUTStride + TxHeaderSize + c * 2) = w >>8 | w <<8;
      w++;
    }*/
    for (uint16_t c = 0; c < nrOuts; ++c)
    {
      *(p + scTxHeaderSize + c * 3) = 0;
      *(p + scTxHeaderSize + c * 3 + 1) = uint8_t(Sine48[i % 48] & 0xff);
      *(p + scTxHeaderSize + c * 3 + 2) = uint8_t(Sine48[i % 48] >> 8);
    }
#endif
  }
}

//---------------------------------------------------------------------------------------------
static const uint16_t rxpktSize = 1024;
static const uint16_t rxpktCount = 8;
static const uint16_t IsoSize = rxpktCount * rxpktSize;
static const uint16_t precharge = 44;

//must be a power of two
static const uint8_t NrXfers = 2;
inline void NextXfer(uint8_t& val) { ++val &= (NrXfers - 1); }
static const uint8_t NrASIOBuffs = 16;
inline void NextASIO(uint8_t& val) { ++val &= (NrASIOBuffs - 1); }

//---------------------------------------------------------------------------------------------

void CypressDevice::main()
{
  LOG0("CypressDevice::main");
  if (mDevHandle == INVALID_HANDLE_VALUE)
    //signal the parent of the thread failure
    return;

  mExitHandle = CreateEvent(NULL, TRUE, FALSE, NULL);
  ResetEvent(mExitHandle);

  DWORD proAudioIndex = 0;
  HANDLE AvrtHandle = AvSetMmThreadCharacteristics(L"Pro Audio", &proAudioIndex);
  AvSetMmThreadPriority(AvrtHandle, AVRT_PRIORITY_CRITICAL);


  const uint32_t nrIns = (devParams[NrIns].val + 1) * 2;
  const uint32_t nrOuts = (devParams[NrOuts].val + 1) * 2;
  nrSamples = devParams[NrSamples].val;
  const uint32_t fifoDepth = devParams[FifoDepth].val;

  InStride = nrIns * 3;
  INBuffSize = (InStride * nrSamples);

  OUTStride = scTxHeaderSize + (nrOuts * 3);
  OUTBuffSize = OUTStride * nrSamples;

  ZeroMemory(&mDevStatus, sizeof(mDevStatus));

  // configure the fpga channel params
  union {
    struct { uint32_t
      outs : 4,
       ins : 4,
   samples : 8,
      fifo : 8,
   padding : 8;
    };
    uint32_t u32;
  } ch_params = {
    (uint32_t)devParams[NrOuts].val , (uint32_t)devParams[NrIns].val, nrSamples, fifoDepth, 0
  };

  uint8_t* mINBuff = nullptr, * mOUTBuff = nullptr;
  devClient.AllocBuffers(INBuffSize * 2, mINBuff, OUTBuffSize * 2, mOUTBuff);

  uint8_t* inPtr[NrASIOBuffs];
  uint8_t* outPtr[NrASIOBuffs];
  asioInPtr = inPtr;
  asioOutPtr = outPtr;
  for (uint32_t c = 0; c < NrASIOBuffs; ++c)
  {
    inPtr[c] = mINBuff + (c* INBuffSize);
    outPtr[c] = mOUTBuff + (c * OUTBuffSize);
    InitTxHeaders(outPtr[c], nrSamples);
  }

  XferReq RxRequests[NrXfers];
  XferReq TxRequests[NrXfers];
  ZeroMemory(RxRequests, sizeof(mRxRequests));
  ZeroMemory(TxRequests, sizeof(TxRequests));
  mRxRequests = RxRequests;
  mTxRequests = TxRequests;

  //CALL PROC
  midi.Init();

  auto InitFpga = [this](uint32_t params)
  {
    uint32_t status = ztex_xlabs_init_fifos(mDevHandle);
    status = ztex_default_lsi_set1(mDevHandle, 4, params);
    return status;
  };

  InitFpga(ch_params.u32);

  for (uint32_t c = 0; c < NrXfers; ++c)
  {
    mTxRequests[c].handle = mDevHandle;
    mTxRequests[c].endpoint = mDefOutEP;
    mTxRequests[c].bufflen = IsoSize;

    if (bknd_init_write_xfer(mDevHandle, &mTxRequests[c], rxpktCount, rxpktSize)) {
      mTxRequests[c].ovlp.hEvent = CreateEvent(NULL, FALSE, FALSE, nullptr);
      ZeroMemory(mTxRequests[c].buff, IsoSize);
      InitTxHeaders(mTxRequests[c].buff, precharge);
      bknd_iso_write(&mTxRequests[c]);
    }

    mRxRequests[c].handle = mDevHandle;
    mRxRequests[c].endpoint = mDefInEP;
    mRxRequests[c].bufflen = IsoSize;

    if (bknd_init_read_xfer(mDevHandle, &mRxRequests[c], rxpktCount, rxpktSize)) {
      mRxRequests[c].ovlp.hEvent = CreateEvent(NULL, FALSE, FALSE, nullptr);
      bknd_iso_read(&mRxRequests[c]);
    }
  }

  mDevStatus.LastSR = -1;

  /*The shared mem is a single linear space where we put samples for the asio client
    and where the asio client puts output samples.
    The rxptr is the start point where isoch can write the input data
    The asioptr is the sample point for the asio client
    The txptr is the sample point where we have audio samples for the output data

    |-------|-------------|-------------------|----------------|
    0     txptr         asioptr             rxptr             len

    Each completion will process and advance the pointers until they wrap around.
    To keep the communication with the asio client simple, no buffer to asio will cross the wrap around boundary
  */
  RxProgress = 0;
  RxBuff = 0;
  AsioBuff = 0;
  TxBuff = 0;
  TxBuffPos = 0;
  IsoTxSamples = 0;
  ClientActive = false;

  mTxReqIdx = 0;
  mRxReqIdx = 0;
  mASIOHandle = devClient.GetSwitchHandle();
  ResetEvent(mASIOHandle);


  HANDLE timerH = CreateWaitableTimer(NULL, FALSE, nullptr);
  LARGE_INTEGER li;
  li.QuadPart = -10 *1000000;
  SetWaitableTimer(timerH, &li, 1000, NULL, NULL, false);
  
  bool ErrorBreak = false;

  while (WaitForSingleObject(mExitHandle, 0) == WAIT_TIMEOUT)
  {

    HANDLE events[4] = { mTxRequests[mTxReqIdx].ovlp.hEvent, mRxRequests[mRxReqIdx].ovlp.hEvent, timerH, mASIOHandle };
    DWORD wfmo = WaitForMultipleObjects(mASIOHandle == NULL ? 3 : 4, events, false, 500);

    switch (wfmo)
    {
    case WAIT_OBJECT_0:     TxIsochCB();    break;//tx iso
    case WAIT_OBJECT_0 + 1: RxIsochCB();    break;//rx isoch
    case WAIT_OBJECT_0 + 2: TimerCB(); break;//1sec timer
    case WAIT_OBJECT_0 + 3: AsioClientCB(); break;//ASIO ready

    default: //not good
      if (wfmo == 0xffffffff)
      {
        LOGN("Wait error 0x%08X GetLastError:0x%08X\n", wfmo, GetLastError());
        Sleep(100);//otherwise we will block the universe
      }
      else
      {
        LOGN("Wait failed 0x%08X\n", wfmo);
        ErrorBreak = true;
        SetEvent(mExitHandle);
      }
      break;
    };
  }

  CancelWaitableTimer(timerH);
  CloseHandle(timerH);

  devClient.FreeBuffers(mINBuff, mOUTBuff);

  if (ErrorBreak)
  {
    bknd_abort_pipe(mDevHandle, mDefOutEP);
    bknd_abort_pipe(mDevHandle, mDefInEP);
  }

  for (uint32_t c = 0; c < NrXfers; ++c){
    WaitForSingleObject(mRxRequests[c].ovlp.hEvent, 500);
    CloseHandle(mRxRequests[c].ovlp.hEvent);
    bknd_xfer_cleanup(&mRxRequests[c]);
    WaitForSingleObject(mTxRequests[c].ovlp.hEvent, 500);
    CloseHandle(mTxRequests[c].ovlp.hEvent);
    bknd_xfer_cleanup(&mTxRequests[c]);
  }

  AvRevertMmThreadCharacteristics(AvrtHandle);

  CloseHandle(mExitHandle);
  mExitHandle = INVALID_HANDLE_VALUE;
  LOG0("CypressDevice::main Exit");
}


//---------------------------------------------------------------------------------------------

void CypressDevice::UpdateClient()
{
  devClient.Switch(0, InStride, asioInPtr[AsioBuff], OUTStride, asioOutPtr[AsioBuff] + scTxHeaderSize);
}

//---------------------------------------------------------------------------------------------
static uint32_t sSampleCounter = 0;
void CypressDevice::TxIsochCB()
{
        XferReq& TxReq = mTxRequests[mTxReqIdx];
        uint16_t txIsoSize = IsoSize;
        uint8_t* ptr = TxReq.buff;

        if (uint8_t s = midi.MidiOut(ptr))
        {
          ptr += s;
          txIsoSize -= s;
        }

        uint16_t TxSamples = min(IsoTxSamples, txIsoSize / OUTStride);

        //partial TxBuff
        if (TxSamples > 0 && TxBuffPos > 0 && TxBuff != AsioBuff)
        {
          uint32_t count = min(nrSamples - TxBuffPos, TxSamples);
          memcpy(ptr, asioOutPtr[TxBuff] + (TxBuffPos * OUTStride), count * OUTStride);
          TxBuffPos += count;

          ASSERT(TxBuffPos <= nrSamples);

          if (TxBuffPos >= nrSamples)
          {
            TxBuffPos = 0;
            NextASIO(TxBuff);
          }

          ptr += count * OUTStride;
          IsoTxSamples -= count;
          TxSamples -= count;
        }

        //whole TxBuff

        while (TxSamples >= nrSamples && TxBuff != AsioBuff)
        {
          memcpy(ptr, asioOutPtr[TxBuff], OUTBuffSize);
          NextASIO(TxBuff);

          ptr += OUTBuffSize;
          IsoTxSamples -= nrSamples;
          TxSamples -= nrSamples;
        }
        //Partial TxBuff
        if (TxSamples > 0 && TxBuff != AsioBuff)
        {
          uint32_t count = min(nrSamples - TxBuffPos, TxSamples);
          memcpy(ptr, asioOutPtr[TxBuff] + (TxBuffPos * OUTStride), count * OUTStride);
          ptr += count * OUTStride;
          TxBuffPos += count;
          TxSamples -= count;
          IsoTxSamples -= count;
        }

        // silence samples
        if (TxSamples && !ClientActive)
        {
          InitTxHeaders(ptr, TxSamples);
          ptr += TxSamples * OUTStride;
          IsoTxSamples -= TxSamples;
          //LOGN("SILENCE!!!! %u\r", TxSamples);
        }

        //ASSERT(IsoTxSamples == 0);

        //zero the rest of the buffer
        ZeroMemory(ptr, (TxReq.buff + txIsoSize) - ptr);

        bknd_iso_write(&TxReq);
        NextXfer(mTxReqIdx);
}

//---------------------------------------------------------------------------------------------

void CypressDevice::TimerCB()
{

  LOGN(" %u Samples/sec\r", sSampleCounter);
  sSampleCounter = 0;
}

//---------------------------------------------------------------------------------------------

void CypressDevice::RxIsochCB()
{
        XferReq& RxReq = mRxRequests[mRxReqIdx];
        for (uint32_t i = 0; i < rxpktCount; i++)
        {
          IsoReqResult result = bknd_iso_get_result(&RxReq, i);
          if (result.status == 0 && result.length > 0) //a filled block
          {
            uint8_t* ptr = RxReq.buff + (i * rxpktSize);
            uint16_t len = 0;
            //-------------------------------------------------
            if (ProcessHdr(ptr)) {
              ptr += sizeof(RxHeader);
              len = (uint16_t)(result.length - sizeof(RxHeader));
              uint16_t samples = len / InStride;
              IsoTxSamples += samples;

              sSampleCounter += samples;

              if ((len + RxProgress) <= INBuffSize) {
                memcpy(asioInPtr[RxBuff] + RxProgress, ptr, len);
                RxProgress += len;
                len = 0;
              }
              else {
                memcpy(asioInPtr[RxBuff] + RxProgress, ptr, INBuffSize - RxProgress);
                len -= INBuffSize - RxProgress;
                ptr += INBuffSize - RxProgress;
                RxProgress = INBuffSize;
              }
            }
            else
            {
              LOG0("ISOCH Rx buff malformed!");
            }

            if (RxProgress == INBuffSize) {//Packet complete, dispatch to client
              uint8_t next = RxBuff;
              NextASIO(next);
              if (ClientActive) {

                if (RxBuff == AsioBuff)
                  UpdateClient();

                if (next != TxBuff)
                  RxBuff = next;
                else
                {
                  ClientActive = devClient.ClientPresent();
                  LOG0("ASIO queue full!");
                }
              }
              else
              {//just keep filling new ones 
                AsioBuff = RxBuff;
                TxBuff = RxBuff;
                RxBuff = next;
                TxBuffPos = 0;
              }

              RxProgress = len;
              memcpy(asioInPtr[RxBuff], ptr, len);
            }

            if (RxProgress > INBuffSize) //missed something, start again
            {
              mDevStatus.ResyncErrors++;
              LOGN("Missed %u ISOCH packets\n", mDevStatus.ResyncErrors);
              RxProgress = 0;
            }
          }
        }
        //fire again
        bknd_iso_read(&RxReq);
        NextXfer(mRxReqIdx);
}

//---------------------------------------------------------------------------------------------

void CypressDevice::AsioClientCB()
{
        bool present = devClient.ClientPresent();
        if (present) {

          if (ClientActive)
            NextASIO(AsioBuff);

          if (!ClientActive || AsioBuff != RxBuff)
            UpdateClient();
        }
        else
        {
          AsioBuff = RxBuff;
          TxBuff = RxBuff;
          TxBuffPos = 0;
        }

        ClientActive = present;
}

//---------------------------------------------------------------------------------------------

bool CypressDevice::GetStatus(UsbDeviceStatus & status)
{
  if (mDevHandle != INVALID_HANDLE_VALUE) {
    if (mExitHandle != INVALID_HANDLE_VALUE) {
      status = mDevStatus;
    } else {
      int64_t result = ztex_default_lsi_get1(mDevHandle, 2);
      if (result < 0) {
        return false;
      } else {
        ZeroMemory(&status, sizeof(UsbDeviceStatus));
        status.LastSR = (uint32_t)result;
      }
    }
    return true;
  }
  return false;
}

//---------------------------------------------------------------------------------------------

uint32_t CypressDevice::GetSampleRate()
{
  uint32_t lastSR = (uint32_t)(-1);
  if (mDevHandle != INVALID_HANDLE_VALUE) {
    if (mExitHandle != INVALID_HANDLE_VALUE) {
      lastSR = mDevStatus.LastSR;
    } else {
      int64_t result = ztex_default_lsi_get1(mDevHandle, 2);
      if (result > 0)
        lastSR = ConvertSampleRate((uint32_t)result);
    }
  }
  return lastSR;
}

