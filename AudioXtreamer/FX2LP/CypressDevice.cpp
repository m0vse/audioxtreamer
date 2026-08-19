#include "stdafx.h"

#include <process.h>
#include <stdio.h>


#include "CypressDevice.h"

#include "UsbBackend.h"

#include "ZTEXDev\ztexdev.h"
#include "resource.h"
#include "AudioXtreamer\AppLog.h"


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

namespace
{
  const uint32_t FpgaCookie = 0x47545254; // "TRTG" in the LSI register byte order
  const uint32_t FpgaInterfaceVersion = 4;
  const uint8_t FpgaSettingsRegister = 4;
  const uint32_t FpgaControlMute = 0x00000080;
  const LONG OutputGainUnity = 256;

  int SetFpgaMuteVerified(HANDLE handle, bool mute, uint32_t& readback)
  {
    int lastStatus = -1;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
      const int64_t current = ztex_default_lsi_get1(handle, FpgaSettingsRegister);
      if (current < 0)
      {
        lastStatus = static_cast<int>(current);
        continue;
      }

      uint32_t desired = static_cast<uint32_t>(current);
      if (mute)
        desired |= FpgaControlMute;
      else
        desired &= ~FpgaControlMute;

      // The XLabs firmware requires vendor request 0x70 before each LSI
      // register change. It also resets the USB FIFOs, so callers must only
      // change mute while transfers are stopped or are about to be stopped.
      lastStatus = ztex_xlabs_init_fifos(handle);
      if (lastStatus < 0)
        continue;

      lastStatus = ztex_default_lsi_set1(handle, FpgaSettingsRegister, desired);
      if (lastStatus < 0)
        continue;

      const int64_t verified = ztex_default_lsi_get1(handle, FpgaSettingsRegister);
      if (verified < 0)
      {
        lastStatus = static_cast<int>(verified);
        continue;
      }

      readback = static_cast<uint32_t>(verified);
      if ((readback & FpgaControlMute) == (desired & FpgaControlMute))
        return 0;

      lastStatus = -1000; // the LSI transaction succeeded but the FPGA did not retain the bit
      Sleep(1);
    }
    return lastStatus;
  }
}

uint8_t setyb[256];

CypressDevice::CypressDevice(UsbDeviceClient & client )
  : UsbDevice(client)
  , asioInPtr(nullptr)
  , asioOutPtr(nullptr)
  , mDevStatus({ 0 })
  , mFileHandle(NULL)
  , mASIOHandle(NULL)
  , mTxRequests(nullptr)
  , mRxRequests(nullptr)
  , mLastLoggedStatus({ 0 })
  , mLastOpenFailureLog(0)
  , mLastFpgaProgramAttempt(0)
  , mQueueFullLogged(false)
  , mMuteRequestHandle(NULL)
  , mMuteAckHandle(NULL)
  , mMuteCommand(-1)
  , mMuteRequestSucceeded(FALSE)
  , mOutputGain(0)
  , mOutputGainTarget(0)
  , mFpgaMuted(TRUE)
  , mAutoUnmutePending(false)
  , mOutputSignalLogged(false)
  , mOutputSilenceLogged(false)
  , mOutputSignalStart(0)
{
  LOG0("CypressDevice::CypressDevice");

  mDefOutEP = 0;
  mDefInEP = 0;
  mDevHandle = INVALID_HANDLE_VALUE;
  hth_Worker = INVALID_HANDLE_VALUE;
  mExitHandle = INVALID_HANDLE_VALUE;
  mMuteRequestHandle = CreateEvent(NULL, FALSE, FALSE, NULL);
  mMuteAckHandle = CreateEvent(NULL, FALSE, FALSE, NULL);


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
    const uint32_t resourceSize = SizeofResource(NULL, hrc);
    const uint32_t prefixSize = 512;
    const uint32_t usbPacketSize = 64;
    // The FX2 configuration path needs a short final USB packet to clock the
    // Spartan-6 through startup. Add one trailing zero clock byte only when
    // the prefixed bitstream would otherwise end exactly on a packet boundary.
    const uint32_t suffixSize = ((prefixSize + resourceSize) % usbPacketSize) == 0 ? 1 : 0;
    mResourceSize = prefixSize + resourceSize + suffixSize;

    uint8_t * bitstream = (uint8_t *)malloc(mResourceSize);
    ZeroMemory(bitstream, mResourceSize);

    uint8_t* buf = bitstream + prefixSize;
    for (uint32_t c = 0; c < resourceSize; c++, buf++) {
      register uint8_t b = bits[c];
      *buf = setyb[b];
    }

    mBitstream = bitstream;
    AppLog::Info("FPGA", "Embedded FPGA bitstream loaded (%u bytes; trailing clock byte=%s)",
      mResourceSize, suffixSize ? "yes" : "no");
  }
  else
    AppLog::Error("FPGA", "Embedded FPGA bitstream resource is missing");
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
  if (mMuteRequestHandle)
    CloseHandle(mMuteRequestHandle);
  if (mMuteAckHandle)
    CloseHandle(mMuteAckHandle);
}

bool CypressDevice::Start()
{
  LOG0("CypressDevice::Start");
  if (mDevHandle == INVALID_HANDLE_VALUE) {
    LogOpenFailure("Cannot start because the USB device is not open");
    return false;
  }
  //let's try to identify the fpga
  int64_t get_result = ztex_default_lsi_get1(mDevHandle, 0);
  if (get_result != FpgaCookie) {
    LogOpenFailure("FPGA identity check failed (TRTG cookie not found)");
    return false;
  }
  get_result = ztex_default_lsi_get1(mDevHandle, 1);
  if (get_result != FpgaInterfaceVersion) {
    LogOpenFailure("FPGA interface version is incompatible");
    return false;
  }


  if (hth_Worker != INVALID_HANDLE_VALUE) {
    DWORD dwExCode;
    GetExitCodeThread(hth_Worker, &dwExCode);
    if (dwExCode == STILL_ACTIVE)
      return true;
  }

  InterlockedExchange(&mOutputGain, 0);
  InterlockedExchange(&mOutputGainTarget, 0);
  mAutoUnmutePending = true;

  hth_Worker = (HANDLE)_beginthread(StaticWorkerThread, 0, this);
  if (hth_Worker != INVALID_HANDLE_VALUE) {
    ::SetThreadPriority(hth_Worker, THREAD_PRIORITY_TIME_CRITICAL);
    AppLog::Info("Stream", "USB worker thread created");
    return true;
  }
  AppLog::Error("Stream", "Could not create the USB worker thread");
  return false;
}

bool CypressDevice::Stop(bool wait)
{
  LOG0("CypressDevice::Stop");


  if (mExitHandle != INVALID_HANDLE_VALUE) {
    InterlockedExchange(&mOutputGainTarget, 0);
    const DWORD fadeStart = GetTickCount();
    while (InterlockedCompareExchange(&mOutputGain, 0, 0) != 0 &&
           GetTickCount() - fadeStart < 100)
      Sleep(1);

    if (InterlockedCompareExchange(&mOutputGain, 0, 0) != 0)
      AppLog::Warning("Stream", "Output fade-down did not complete before its 100 ms timeout");
    else
      Sleep(15); // allow queued USB/FIFO samples at zero gain to reach the pins

    if (!RequestFpgaMute(true, 250))
      AppLog::Warning("FPGA", "Could not confirm output mute before stopping the stream");

    BOOL result = SetEvent(mExitHandle);
    if (result == 0) {
      LOG0("CypressDevice::Stop SetEvent Exit FAILED!");
      AppLog::Error("Stream", "Could not signal the USB worker to stop (Windows error %lu)", GetLastError());
      wait = false;
    }

    if (wait) {
      WaitForSingleObject(hth_Worker, INFINITE);
      hth_Worker = INVALID_HANDLE_VALUE;
    }

    AppLog::Info("Stream", "USB worker stop requested%s", wait ? " and completed" : "");
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
  int status = 1;
  bool compatible = false;
  bool programmed = false;
  int64_t fpgaCookie = -1;
  int64_t fpgaVersion = -1;
  int identityAttempt = 0;
  int identityAttempts = 1;
  mDefOutEP = 0;
  mDefInEP = 0;
  memset(&info, 0, sizeof(ztex_device_info));
  const char* failure = "AudioXtreamer WinUSB interface was not found";

  if (!bknd_open(handle, mFileHandle))
    goto err;

  status = ztex_get_device_info(handle, &info);
  if (status < 0) {
    failure = "Could not read ZTEX device information";
    fprintf(stderr, "Error: Unable to get device info\n");
    goto err;
  }

  mDefInEP = 0x82;//info.default_in_ep;
  mDefOutEP = 0x8;//info.default_out_ep;

  status = ztex_get_fpga_config(handle);
  if (status > 0)
  {
    fpgaCookie = ztex_default_lsi_get1(handle, 0);
    fpgaVersion = ztex_default_lsi_get1(handle, 1);
    compatible = fpgaCookie == FpgaCookie && fpgaVersion == FpgaInterfaceVersion;
  }

  if (!compatible)
  {
    const DWORD now = GetTickCount();
    if (mLastFpgaProgramAttempt != 0 && now - mLastFpgaProgramAttempt < 5000) {
      failure = "FPGA image is incompatible or not responding; waiting before another download";
      goto err;
    }
    mLastFpgaProgramAttempt = now;
    AppLog::Info("FPGA", "Loading embedded FPGA image (configured=%s, detected interface version=%lld)",
      status > 0 ? "yes" : "no", fpgaVersion);

   if (mBitstream != nullptr) {
#define EP0_TRANSACTION_SIZE 2048

      // reset FPGA
      status = (BOOL)control_transfer(handle, 0x40, 0x31, 0, 0, NULL, 0, 1500);
      if (status != 0) {
        failure = "FPGA reset request failed";
        goto err;
      }
      // transfer data

      uint32_t last_idx = mResourceSize % EP0_TRANSACTION_SIZE;
      int bufs_idx = (mResourceSize / EP0_TRANSACTION_SIZE);

      for (int i = 0; (status >= 0) && (i < bufs_idx); i++)
      {
        status = (BOOL)control_transfer(handle, 0x40, 0x32, 0, 0, mBitstream + (i * EP0_TRANSACTION_SIZE), EP0_TRANSACTION_SIZE, 1500);
        if (EP0_TRANSACTION_SIZE != status) {
          failure = "FPGA bitstream transfer failed";
          goto err;
        }
      }

      if (last_idx)
      {
        status = (BOOL)control_transfer(handle, 0x40, 0x32, 0, 0, mBitstream + bufs_idx * EP0_TRANSACTION_SIZE, last_idx, 1500);
        if (last_idx != status) {
          failure = "Final FPGA bitstream transfer failed";
          goto err;
        }
      }

    } else {
     failure = "No FPGA bitstream is embedded in the application";
     goto err;
    }

    fflush(stderr);
    // check config
    status = ztex_get_fpga_config(handle);
    if (status < 0) {
      failure = "Could not read FPGA configuration state";
      fprintf(stderr, "Error: Unable to get FPGA configuration state\n");
      goto err;
    }
    else if (status == 0) {
      failure = "FPGA rejected the downloaded bitstream";
      fprintf(stderr, "Error: FPGA not configured\n");
      goto err;
    }
    programmed = true;
  }
  else
  {
    AppLog::Info("FPGA", "Reusing compatible FPGA image (interface version %u)", FpgaInterfaceVersion);
  }

  identityAttempts = 100;
  for (identityAttempt = 0; identityAttempt < identityAttempts; ++identityAttempt)
  {
    Sleep(10);
    fpgaCookie = ztex_default_lsi_get1(handle, 0);
    fpgaVersion = ztex_default_lsi_get1(handle, 1);
    if (fpgaCookie == FpgaCookie && fpgaVersion == FpgaInterfaceVersion)
      break;
  }
  if (fpgaCookie != FpgaCookie || fpgaVersion != FpgaInterfaceVersion) {
    AppLog::Warning("FPGA", "Identity verification did not settle after %d attempt(s): cookie=0x%08llX version=0x%08llX",
      identityAttempts, fpgaCookie, fpgaVersion);
    failure = "FPGA identity or interface version verification failed";
    goto err;
  }

  // LSI writes are not reliable until the streaming alternate interface and
  // FIFOs have been initialised by the worker. Keep the desired state muted;
  // main() writes and verifies the muted configuration before starting I/O.
  InterlockedExchange(&mFpgaMuted, TRUE);

  status = 0;
  goto noerr;

err:
  status = 1;
  LogOpenFailure(failure);
  if (handle != INVALID_HANDLE_VALUE)
    bknd_close(handle, mFileHandle);

noerr:

  mDevHandle = status == 0 ? handle : INVALID_HANDLE_VALUE;
  if (status == 0) {
    AppLog::Info("FPGA", "%s FPGA configuration verified; mute will be verified before streaming",
      programmed ? "New" : "Existing");
    return true;
  }
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
    AppLog::Info("Device", "USB device closed");
  }

  return true;
}

void CypressDevice::LogOpenFailure(const char* reason)
{
  const DWORD now = GetTickCount();
  if (mLastOpenFailureLog == 0 || now - mLastOpenFailureLog >= 10000)
  {
    AppLog::Warning("Device", "%s; retrying", reason);
    mLastOpenFailureLog = now;
  }
}

bool CypressDevice::SetFpgaMute(bool mute)
{
  if (mDevHandle == INVALID_HANDLE_VALUE)
    return false;

  const LONG requested = mute ? TRUE : FALSE;
  uint32_t readback = 0;
  const int status = SetFpgaMuteVerified(mDevHandle, mute, readback);
  if (status < 0) {
    AppLog::Error("FPGA", "Could not verify output %s (status %d, register 4 readback 0x%08X)",
      mute ? "mute" : "unmute", status, readback);
    return false;
  }

  InterlockedExchange(&mFpgaMuted, requested);
  AppLog::Info("FPGA", "Outputs %s (verified register 4=0x%08X)",
    mute ? "muted" : "unmuted", readback);
  return true;
}

bool CypressDevice::RequestFpgaMute(bool mute, DWORD timeoutMs)
{
  const LONG requested = mute ? TRUE : FALSE;
  if (InterlockedCompareExchange(&mFpgaMuted, requested, requested) == requested)
    return true;
  if (!mMuteRequestHandle || !mMuteAckHandle)
    return false;

  ResetEvent(mMuteAckHandle);
  InterlockedExchange(&mMuteRequestSucceeded, FALSE);
  InterlockedExchange(&mMuteCommand, requested);
  if (!SetEvent(mMuteRequestHandle))
    return false;

  return WaitForSingleObject(mMuteAckHandle, timeoutMs) == WAIT_OBJECT_0 &&
    InterlockedCompareExchange(&mMuteRequestSucceeded, FALSE, FALSE) != FALSE;
}

void CypressDevice::MuteRequestCB()
{
  const LONG command = InterlockedExchange(&mMuteCommand, -1);
  const bool succeeded = command >= 0 && SetFpgaMute(command != FALSE);
  InterlockedExchange(&mMuteRequestSucceeded, succeeded ? TRUE : FALSE);
  SetEvent(mMuteAckHandle);
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


#define SNAP_TOLERANCE 100
#define SNAP_TO_AND_RET(val,snapto) { if(val > (snapto - SNAP_TOLERANCE) && val < (snapto + SNAP_TOLERANCE)) return snapto; }

constexpr auto ConvertSampleRate(uint16_t srReg)
{
  uint32_t sr = srReg * 10;

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

//---------------------------------------------------------------------------------------------
static const uint32_t rxpktSize = 1024;
static const uint32_t rxpktCount = 16;
static const uint32_t IsoSize = rxpktCount * rxpktSize;

//must be a power of two
static const uint8_t NrXfers = 2;
inline void NextXfer(uint8_t& val) { ++val &= (NrXfers - 1); }
static const uint8_t NrASIOBuffs = 16;
inline void NextASIO(uint8_t& val) { ++val &= (NrASIOBuffs - 1); }

//---------------------------------------------------------------------------------------------
static const bool AudioOut = true;
static const bool AudioIn  = true;

void CypressDevice::main()
{
  LOG0("CypressDevice::main");
  if (mDevHandle == INVALID_HANDLE_VALUE)
    //signal the parent of the thread failure
    return;

  bool ErrorBreak = false;
  mExitHandle = CreateEvent(NULL, TRUE, FALSE, NULL);
  ResetEvent(mExitHandle);  

  DWORD proAudioIndex = 0;
  HANDLE AvrtHandle = AvSetMmThreadCharacteristics(L"Pro Audio", &proAudioIndex);
  AvSetMmThreadPriority(AvrtHandle, AVRT_PRIORITY_CRITICAL);


  const uint32_t nrIns = (theSettings[NrIns].val + 1) * 2;
  const uint32_t nrOuts = (theSettings[NrOuts].val + 1) * 2;
  nrSamples = theSettings[NrSamples].val;
  const uint32_t fifoDepth = theSettings[FifoDepth].val;

  InStride = nrIns * 3;
  INBuffSize = (InStride * nrSamples);

  OUTStride = (nrOuts * 3);
  OUTBuffSize = OUTStride * nrSamples;

  ZeroMemory(&mDevStatus, sizeof(mDevStatus));
  ZeroMemory(&mLastLoggedStatus, sizeof(mLastLoggedStatus));
  mQueueFullLogged = false;
  AppLog::Info("Stream", "Configuration: inputs=%u outputs=%u buffer=%u samples FIFO=%u samples",
    nrIns, nrOuts, nrSamples, fifoDepth);

  // configure the fpga channel params
  union {
    struct { uint32_t
      outs : 8,
       ins : 8,
      fifo : 8,
   padding : 8;
    };
    uint32_t u32;
  } ch_params = {
    nrOuts, nrIns, fifoDepth, 0
  };
  if (InterlockedCompareExchange(&mFpgaMuted, FALSE, FALSE) != FALSE)
    ch_params.u32 |= FpgaControlMute;

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
  }

  XferReq RxRequests[NrXfers];
  XferReq TxRequests[NrXfers];
  ZeroMemory(RxRequests, sizeof(RxRequests));
  ZeroMemory(TxRequests, sizeof(TxRequests));
  mRxRequests = RxRequests;
  mTxRequests = TxRequests;

  bknd_select_alt_ifc(mDevHandle, 0);
  Sleep(100);
  bknd_select_alt_ifc(mDevHandle, 3);
  //INIT the buffers based on the active alternate setting
  for (uint32_t i = 0; i < NrXfers; ++i)
  {
    if (AudioOut) {
      mTxRequests[i].handle = mDevHandle;
      mTxRequests[i].endpoint = mDefOutEP;
      mTxRequests[i].bufflen = IsoSize;
      mTxRequests[i].xtype = eIsochronous;

      if (bknd_init_xfer(mDevHandle, &mTxRequests[i], rxpktCount, rxpktSize)) {
        mTxRequests[i].ovlp.hEvent = CreateEvent(NULL, FALSE, FALSE, nullptr);
        ZeroMemory(mTxRequests[i].buff, IsoSize);

        for (uint8_t c = 0; c < rxpktCount; ++c)
          *(uint32_t*)(mTxRequests[i].buff + (rxpktSize * c)) = 0xaa5555aa;
      }
      else
        AppLog::Error("USB", "Could not initialise output isochronous transfer %u", i);
    }

    if (AudioIn) {
      mRxRequests[i].handle = mDevHandle;
      mRxRequests[i].endpoint = mDefInEP;
      mRxRequests[i].bufflen = IsoSize;
      mRxRequests[i].xtype = eIsochronous;

      if (bknd_init_xfer(mDevHandle, &mRxRequests[i], rxpktCount, rxpktSize)) {
        mRxRequests[i].ovlp.hEvent = CreateEvent(NULL, FALSE, FALSE, nullptr);
      }
      else
        AppLog::Error("USB", "Could not initialise input isochronous transfer %u", i);
    }
  }

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
  ZeroMemory(mFeedbackQueue, sizeof(mFeedbackQueue));
  mFeedbackRead = 0;
  mFeedbackWrite = 0;
  mFeedbackCount = 0;
  ClientActive = false;
  InterlockedExchange(&mOutputGain, 0);
  InterlockedExchange(&mOutputGainTarget, 0);

  mTxReqIdx = 0;
  mRxReqIdx = 0;
  mASIOHandle = devClient.GetSwitchHandle();
  ResetEvent(mASIOHandle);

  ZeroMemory(&mXferEp0Status, sizeof(mXferEp0Status));
  mXferEp0Status.xtype = eControl;
  mXferEp0Status.handle = mDevHandle;
  mXferEp0Status.stp.bmRequestType = 0xc0;
  mXferEp0Status.stp.bRequest = LSI8_READ;
  mXferEp0Status.stp.wValue = 0;
  mXferEp0Status.stp.wIndex = 2;
  mXferEp0Status.stp.wLength = 4;
  mXferEp0Status.bufflen = 4;
  mXferEp0Status.ovlp.hEvent = CreateEvent(NULL, FALSE, FALSE, nullptr);

  bknd_init_xfer(mDevHandle, &mXferEp0Status, 0, 0);


  HANDLE timerH = CreateWaitableTimer(NULL, FALSE, nullptr);
  LARGE_INTEGER li;
  li.QuadPart = -100000; // first status read after 10 ms; subsequent reads every 250 ms
  SetWaitableTimer(timerH, &li, 250, NULL, NULL, false);

  bool streamConfigFailed = false;
  int32_t status = ztex_xlabs_init_fifos(mDevHandle);
  if (status < 0) {
    AppLog::Error("FPGA", "FIFO initialisation failed (status %d)", status);
    streamConfigFailed = true;
  }
  status = ztex_default_lsi_set1(mDevHandle, FpgaSettingsRegister, ch_params.u32);
  if (status < 0) {
    AppLog::Error("FPGA", "Stream configuration write failed (status %d)", status);
    streamConfigFailed = true;
  }
  else if (!SetFpgaMute(true)) {
    AppLog::Error("FPGA", "Muted stream configuration could not be verified");
    streamConfigFailed = true;
  }
  else if (!SetFpgaMute(false)) {
    AppLog::Error("FPGA", "Outputs could not be unmuted before starting USB transfers");
    streamConfigFailed = true;
  }
  else {
    // Unmuting above necessarily resets the FX2 FIFOs. Do it before any
    // isochronous requests are submitted so an already-active ASIO client
    // cannot observe a broken packet stream after Stop/Start.
    mAutoUnmutePending = false;
  }
  if (streamConfigFailed)
  {
    ErrorBreak = true;
    SetEvent(mExitHandle);
  }

  for (uint32_t i = 0; i < NrXfers; ++i)
  {
    if (!streamConfigFailed && AudioOut)
      bknd_iso_write(&mTxRequests[i]);

    if (!streamConfigFailed && AudioIn)
      bknd_iso_read(&mRxRequests[i]);
  }

  mDevStatus.LastSR = -1;

  while (WaitForSingleObject(mExitHandle, 0) == WAIT_TIMEOUT)
  {

    HANDLE events[] = {
      timerH,
      mXferEp0Status.ovlp.hEvent,
      mASIOHandle,
      mMuteRequestHandle,
      mRxRequests[mRxReqIdx].ovlp.hEvent,
      mTxRequests[mTxReqIdx].ovlp.hEvent
    };
    uint8_t event_count = (uint8_t)_countof(events);
    DWORD wfmo = WaitForMultipleObjects( event_count, events, false, 200);

    switch (wfmo)
    {
    case WAIT_OBJECT_0    : TimerCB();      break;// 1/4 sec timer
    case WAIT_OBJECT_0 + 1: Ep0StatusCB();  break;
    case WAIT_OBJECT_0 + 2: AsioClientCB(); break;//ASIO ready
    case WAIT_OBJECT_0 + 3: MuteRequestCB(); break;
    case WAIT_OBJECT_0 + 4: RxIsochCB();    break;//rx isoch
    case WAIT_OBJECT_0 + 5: TxIsochCB();    break;//tx iso
    case WAIT_TIMEOUT:
      LOG0("USB worker timed out waiting for an event");
      break;

    default: //not good
      if (wfmo == 0xffffffff)
      {
        LOGN("Wait error 0x%08X GetLastError:0x%08X\n", wfmo, GetLastError());
        AppLog::Error("USB", "Worker wait failed (Windows error 0x%08lX)", GetLastError());
        Sleep(100);//otherwise we will block the universe
      }
      else
      {
        LOGN("Wait failed 0x%08X\n", wfmo);
        AppLog::Error("USB", "Worker received an unexpected wait result 0x%08lX", wfmo);
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
    bknd_abort_pipe(mDevHandle, 0);
  }

  for (uint32_t c = 0; c < NrXfers; ++c){
    if (AudioIn) {
      WaitForSingleObject(mRxRequests[c].ovlp.hEvent, 500);
      CloseHandle(mRxRequests[c].ovlp.hEvent);
      bknd_xfer_cleanup(&mRxRequests[c]);
    }
    if (AudioOut) {
      WaitForSingleObject(mTxRequests[c].ovlp.hEvent, 500);
      CloseHandle(mTxRequests[c].ovlp.hEvent);
      bknd_xfer_cleanup(&mTxRequests[c]);
    }
  }

  WaitForSingleObject(mXferEp0Status.ovlp.hEvent, 500);
  CloseHandle(mXferEp0Status.ovlp.hEvent);
  bknd_xfer_cleanup(&mXferEp0Status);

  if (ErrorBreak)
    SetFpgaMute(true);

  bknd_select_alt_ifc(mDevHandle, 0); //release the bandwidth

  AvRevertMmThreadCharacteristics(AvrtHandle);

  CloseHandle(mExitHandle);
  mExitHandle = INVALID_HANDLE_VALUE;
  AppLog::Info("Stream", "USB worker thread exited%s", ErrorBreak ? " after an error" : " normally");
  LOG0("CypressDevice::main Exit");
}


//---------------------------------------------------------------------------------------------

void CypressDevice::UpdateClient()
{
  devClient.Switch(0, InStride, asioInPtr[AsioBuff], OUTStride, asioOutPtr[AsioBuff]);
}

//---------------------------------------------------------------------------------------------
static uint32_t sSampleCounter = 0;

//traverses the spp array copying samples from the asio buff to the microframes based on the received distribution
uint32_t DistributeSamples(uint8_t* uf_ptrs[rxpktCount], uint8_t spp[rxpktCount],
                           uint8_t& spNr, uint8_t* src, uint32_t nrSamples,
                           uint32_t outStride)
{
  uint32_t progress = nrSamples;
  while (spNr < rxpktCount && nrSamples > 0)
  {
    if (spp[spNr])
    {
      uint32_t samples = min(nrSamples, (uint32_t)spp[spNr]);
      uint32_t bytes = samples * outStride;
      memcpy(uf_ptrs[spNr], src, bytes);
      src += bytes;
      nrSamples -= samples;
      spp[spNr] -= (uint8_t)samples;

      uf_ptrs[spNr] += bytes;
      if (spp[spNr] == 0)
        spNr++;
    }
    else
      spNr++;
  }
  return progress - nrSamples;
}

void CypressDevice::ApplyOutputGain(uint8_t* buffer, const uint8_t packetSamples[IsoPacketCount])
{
  LONG gain = InterlockedCompareExchange(&mOutputGain, 0, 0);

  for (uint8_t packet = 0; packet < IsoPacketCount; ++packet)
  {
    uint8_t* frame = buffer + (rxpktSize * packet);
    for (uint8_t sampleIndex = 0; sampleIndex < packetSamples[packet]; ++sampleIndex)
    {
      const LONG target = InterlockedCompareExchange(&mOutputGainTarget, 0, 0);
      if (gain < target)
        ++gain;
      else if (gain > target)
        --gain;

      for (uint32_t channelOffset = 0; channelOffset < OUTStride; channelOffset += 3)
      {
        uint8_t* sampleBytes = frame + channelOffset;
        int32_t sample = sampleBytes[0] |
          (static_cast<int32_t>(sampleBytes[1]) << 8) |
          (static_cast<int32_t>(sampleBytes[2]) << 16);
        if (sample & 0x00800000)
          sample -= 0x01000000;

        const int32_t scaled = static_cast<int32_t>((static_cast<int64_t>(sample) * gain) >> 8);
        sampleBytes[0] = static_cast<uint8_t>(scaled);
        sampleBytes[1] = static_cast<uint8_t>(scaled >> 8);
        sampleBytes[2] = static_cast<uint8_t>(scaled >> 16);
      }
      frame += OUTStride;
    }
  }

  InterlockedExchange(&mOutputGain, gain);
}

void CypressDevice::TxIsochCB()
{
        XferReq& TxReq = mTxRequests[mTxReqIdx];
        uint8_t* ptr = TxReq.buff;
        ZeroMemory(ptr, IsoSize);

        uint8_t packetSamples[rxpktCount] = { 0 };
        if (mFeedbackCount > 0)
        {
          memcpy(packetSamples, mFeedbackQueue[mFeedbackRead], sizeof(packetSamples));
          mFeedbackRead = (mFeedbackRead + 1) % FeedbackQueueDepth;
          --mFeedbackCount;
        }

        uint32_t txSamples = 0;
        const uint32_t maxPacketSamples = (rxpktSize - sizeof(uint32_t)) / OUTStride;
        for (uint8_t c = 0; c < rxpktCount; ++c)
        {
          if (packetSamples[c] > maxPacketSamples)
          {
            packetSamples[c] = (uint8_t)maxPacketSamples;
            ++mDevStatus.ResyncErrors;
          }
          txSamples += packetSamples[c];
        }

        uint8_t spp[rxpktCount];
        memcpy(spp, packetSamples, sizeof(spp));
        uint8_t spNr = 0;
        uint8_t* spp_ptr[rxpktCount] = { 0 };
        for (uint8_t c = 0; c < rxpktCount; ++c)
          spp_ptr[c] = ptr + (rxpktSize * c);

        while (txSamples > 0 && ClientActive && TxBuff != AsioBuff)
        {
          uint32_t count = min(nrSamples - TxBuffPos, txSamples);
          uint32_t distributed = DistributeSamples(
            spp_ptr, spp, spNr,
            asioOutPtr[TxBuff] + (TxBuffPos * OUTStride), count, OUTStride);
          if (distributed == 0)
            break;

          TxBuffPos += distributed;
          txSamples -= distributed;
          if (TxBuffPos == nrSamples)
          {
            TxBuffPos = 0;
            NextASIO(TxBuff);
          }
        }

        if (ClientActive && !mOutputSignalLogged)
        {
          bool hasSignal = false;
          for (uint8_t packet = 0; packet < rxpktCount && !hasSignal; ++packet)
          {
            const uint8_t* sample = ptr + (rxpktSize * packet);
            const uint32_t audioBytes = packetSamples[packet] * OUTStride;
            for (uint32_t offset = 0; offset < audioBytes; ++offset)
            {
              if (sample[offset] != 0)
              {
                hasSignal = true;
                break;
              }
            }
          }

          if (hasSignal)
          {
            mOutputSignalLogged = true;
            AppLog::Info("ASIO", "Non-zero host audio reached the USB output path (gain=%ld/256, target=%ld/256)",
              InterlockedCompareExchange(&mOutputGain, 0, 0),
              InterlockedCompareExchange(&mOutputGainTarget, 0, 0));
          }
          else if (!mOutputSilenceLogged && GetTickCount() - mOutputSignalStart >= 2000)
          {
            mOutputSilenceLogged = true;
            AppLog::Warning("ASIO", "Host output buffers have remained silent for two seconds");
          }
        }

        ApplyOutputGain(ptr, packetSamples);

        // WinUSB always transmits the complete registered buffer. Mark the logical
        // end of every microframe so trailing zeroes are not parsed as audio.
        for (uint8_t c = 0; c < rxpktCount; ++c)
        {
          uint32_t bytes = packetSamples[c] * OUTStride;
          *((uint32_t*)(ptr + (rxpktSize * c) + bytes)) = 0xaa5555aa;
        }

        bknd_iso_write(&TxReq);
        NextXfer(mTxReqIdx);
}

//---------------------------------------------------------------------------------------------

void CypressDevice::TimerCB()
{
  static uint8_t count = 0;
  //LOGN(" %u Samples/sec\r", sSampleCounter);
  if (++count == 4)
  {
    count = 0;
    mDevStatus.SwSR = sSampleCounter;
    sSampleCounter = 0;

    if (mDevStatus.ResyncErrors != mLastLoggedStatus.ResyncErrors ||
        mDevStatus.OutSkipCount != mLastLoggedStatus.OutSkipCount ||
        mDevStatus.OutRefillCount != mLastLoggedStatus.OutRefillCount ||
        mDevStatus.InFullCount != mLastLoggedStatus.InFullCount ||
        mDevStatus.Ep6IsoErr != mLastLoggedStatus.Ep6IsoErr)
    {
      AppLog::Warning("Stream",
        "Transport anomaly counters changed: host resync=%u, FPGA output refills=%u, "
        "FPGA empty reads=%u, capture FIFO full=%u, USB capture errors=%u; "
        "output FIFO=%u/256 (target=%u), received samples=%u/s (hardware rate=%u Hz)",
        mDevStatus.ResyncErrors, mDevStatus.OutRefillCount, mDevStatus.OutSkipCount,
        mDevStatus.InFullCount, mDevStatus.Ep6IsoErr, mDevStatus.FifoLevel,
        theSettings[FifoDepth].val, mDevStatus.SwSR, mDevStatus.LastSR);
      mLastLoggedStatus = mDevStatus;
    }
  }

  control_xfer(mXferEp0Status);


  /*LOGN("lsi 0x%02x, 0x%08x\n", 0x20, (uint32_t)get_result);
  uint8_t flags = (uint8_t)get_result;
  for (uint8_t c = 0; c < 7; c++) {
    if (flags & 1)
    {
      get_result = ztex_default_lsi_get1(mDevHandle, 33 + c);
      LOGN("lsi 0x%02x, 0x%08x\n", 0x21 + c, (uint32_t)get_result);
    }
    flags >>= 1;
  } */
}

void CypressDevice::Ep0StatusCB()
{
  if (mXferEp0Status.stp.wIndex == 2)
  {
    struct SampleRateStatus
    {
      uint16_t sr;
      uint16_t fifo;
    } & s = *(struct SampleRateStatus*)mXferEp0Status.buff;

    uint32_t SR = ConvertSampleRate(s.sr);
    if (mDevStatus.LastSR != SR)
    {
      if (mDevStatus.LastSR != -1 && mDevStatus.LastSR != 0)
      {
        AppLog::Warning("Clock", "Hardware sample rate changed from %u Hz to %u Hz", mDevStatus.LastSR, SR);
        devClient.SampleRateChanged();
      }
      else if (SR != 0)
        AppLog::Info("Clock", "Hardware sample rate detected: %u Hz", SR);
      else
        AppLog::Warning("Clock", "Hardware sample rate could not be recognised (raw value %u)", s.sr);
    }

    mDevStatus.LastSR = SR;
    mDevStatus.FifoLevel = s.fifo;
    if (mAutoUnmutePending)
    {
      if (SetFpgaMute(false))
        mAutoUnmutePending = false;
    }
    mXferEp0Status.stp.wIndex = 3;
  }
  else if (mXferEp0Status.stp.wIndex == 3)
  {
    struct FifoErrorStatus
    {
      uint16_t outSkip;
      uint16_t inFull;
    } & s = *(struct FifoErrorStatus*)mXferEp0Status.buff;

    mDevStatus.OutSkipCount = s.outSkip;
    mDevStatus.InFullCount = s.inFull;
    mXferEp0Status.stp.wIndex = 6;
  }
  else
  {
    mDevStatus.OutRefillCount = *reinterpret_cast<uint32_t*>(mXferEp0Status.buff) & 0xffff;
    mXferEp0Status.stp.wIndex = 2;
  }
}

//---------------------------------------------------------------------------------------------

void CypressDevice::RxIsochCB()
{
  XferReq& RxReq = mRxRequests[mRxReqIdx];
  uint8_t packetSamples[rxpktCount] = { 0 };
  for (uint32_t i = 0; i < rxpktCount; i++)
  {
    IsoReqResult result = bknd_iso_get_result(&RxReq, i);
    if (result.status == 0 && result.length > 0) //a filled block
    {
      uint8_t* ptr = RxReq.buff + (i * rxpktSize);
      uint32_t len = (uint32_t)result.length;
      if (len > rxpktSize)
      {
        len = rxpktSize;
        ++mDevStatus.ResyncErrors;
      }
      if (len % InStride)
      {
        len -= len % InStride;
        ++mDevStatus.ResyncErrors;
      }
      uint32_t samples = len / InStride;
      packetSamples[i] = (uint8_t)samples;

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

      if (RxProgress == INBuffSize) {//Packet complete, dispatch to client
        uint8_t next = RxBuff;
        NextASIO(next);
        if (ClientActive) {

          if (RxBuff == AsioBuff)
            UpdateClient();

          if (AudioOut == false || next != TxBuff) {
            RxBuff = next;
            mQueueFullLogged = false;
          }
          else
          {
            ClientActive = devClient.ClientPresent();
            LOG0("ASIO queue full!");
            if (!mQueueFullLogged)
            {
              AppLog::Warning("ASIO", "Audio buffer queue became full");
              mQueueFullLogged = true;
            }
          }
        }
        else
        {//just keep filling new ones 
          AsioBuff = RxBuff;
          TxBuff = RxBuff;
          RxBuff = next;
          TxBuffPos = 0;
          RxProgress = 0;
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
    else if (result.status != 0)
    {
      ++mDevStatus.Ep6IsoErr;
    }
  }

  if (mFeedbackCount == FeedbackQueueDepth)
  {
    mFeedbackRead = (mFeedbackRead + 1) % FeedbackQueueDepth;
    --mFeedbackCount;
    ++mDevStatus.ResyncErrors;
  }
  memcpy(mFeedbackQueue[mFeedbackWrite], packetSamples, sizeof(packetSamples));
  mFeedbackWrite = (mFeedbackWrite + 1) % FeedbackQueueDepth;
  ++mFeedbackCount;

  //fire again
  bknd_iso_read(&RxReq);
  NextXfer(mRxReqIdx);
}

//---------------------------------------------------------------------------------------------

void CypressDevice::AsioClientCB()
{
        const bool wasActive = ClientActive;
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
        if (wasActive != ClientActive)
        {
          InterlockedExchange(&mOutputGainTarget, ClientActive ? OutputGainUnity : 0);
          if (ClientActive)
          {
            mOutputSignalLogged = false;
            mOutputSilenceLogged = false;
            mOutputSignalStart = GetTickCount();
          }
          AppLog::Info("ASIO", "Streaming client %s", ClientActive ? "connected" : "disconnected");
        }
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

//---------------------------------------------------------------------------------------------

ASIOSettings::StreamInfo CypressDevice::GetStreamInfo()
{
  ASIOSettings::StreamInfo i;
  ZeroMemory(&i, sizeof(i));
  return i;
}

