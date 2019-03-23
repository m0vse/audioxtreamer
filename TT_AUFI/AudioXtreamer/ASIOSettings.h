#pragma once

namespace ASIOSettings
{
  enum Setting{
    NrIns = 0,
    NrOuts = 1,
    NrSamples = 2,
    FifoDepth = 3,
    MaxSetting = 4
  };

  typedef struct _Settings {
    int val;
    const int def;
    const int max;
    const LPCTSTR key;
    const LPCTSTR desc;
  } Settings[MaxSetting];

#pragma pack(push,1)

  typedef struct _StreamInfo {
    uint32_t RxStride;
    uint32_t RxOffset;
    uint32_t TxStride;
    uint32_t TxOffset;
    uint32_t Flags;
  } StreamInfo;

#pragma pack(pop)

  static const uint8_t ChanEntires = 16;
};

#define WM_XTREAMER WM_APP + 100

extern CLSID IID_TORTUGASIO_XTREAMER;
extern LPCTSTR szNameShMem;
extern LPCTSTR szNameMutex;
extern LPCTSTR szNameClass;
extern LPCTSTR szNameApp;

extern ASIOSettings::Settings theSettings;
