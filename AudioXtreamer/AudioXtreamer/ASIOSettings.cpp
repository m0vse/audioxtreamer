#include "stdafx.h"
#include "ASIOSettings.h"

// class id
// {25CBA31C - 951A - 48C6 - B513 - 012E1E2D09D8}
const CLSID IID_TORTUGASIO_XTREAMER = { 0x25CBA31C, 0x951A, 0x48C6,{ 0xB5, 0x13, 0x01, 0x2E, 0x1E, 0x2D, 0x09, 0xD8 } };

LPCTSTR const szNameShMem = _T("AudioXtreamer_{25CBA31C-951A-48C6-B513-012E1E2D09D8}_Mem");
LPCTSTR const szNameAsioEvent = _T("AudioXtreamer_{25CBA31C-951A-48C6-B513-012E1E2D09D8}_AsioEvent");
LPCTSTR const szNameXtreamerEvent = _T("AudioXtreamer_{25CBA31C-951A-48C6-B513-012E1E2D09D8}_XtreamerEvent");
LPCTSTR const szNameClass = _T("AudioXtreamer_{25CBA31C-951A-48C6-B513-012E1E2D09D8}_Class");
LPCTSTR const szNameApp   = _T("TortugASIO Xtreamer");

ASIOSettings::Settings theSettings =
{
  { 11, 11, 11  ,_T("NrIns"),     _T("; zero index based number of pcm LR lines")},
  { 8, 8, 8     ,_T("NrOuts"),    _T("; zero index based number of pcm LR lines")},
  { 512, 512, 1024,_T("NrSamples"), _T("; ASIO buffer size: power of two from 16 to 1024")},
  { 64, 64, 240 ,_T("FifoSize"),   _T("; Size of the hardware Out FIFO , multiple of 16")}
};

namespace ASIOSettings
{
  namespace
  {
    const int SampleCounts[SampleCountEntryCount] = { 16, 32, 64, 128, 256, 512, 1024 };
    const int FifoStep = 16;
    const int FifoPositionMin = 1;
    const int FifoPositionMax = 15;
  }

  int SampleCountFromPosition(int position)
  {
    if (position < 0)
      position = 0;
    else if (position >= SampleCountEntryCount)
      position = SampleCountEntryCount - 1;
    return SampleCounts[position];
  }

  int SampleCountToPosition(int samples)
  {
    const int normalized = NormalizeSampleCount(samples);
    for (int position = 0; position < SampleCountEntryCount; ++position)
      if (SampleCounts[position] == normalized)
        return position;
    return SampleCountEntryCount - 1;
  }

  int NormalizeSampleCount(int samples)
  {
    // Round unsupported saved values upward so loading an old configuration
    // never silently reduces its timing headroom (for example 350 -> 512).
    for (int position = 0; position < SampleCountEntryCount; ++position)
      if (samples <= SampleCounts[position])
        return SampleCounts[position];
    return SampleCounts[SampleCountEntryCount - 1];
  }

  int FifoDepthFromPosition(int position)
  {
    if (position < FifoPositionMin)
      position = FifoPositionMin;
    else if (position > FifoPositionMax)
      position = FifoPositionMax;
    return position * FifoStep;
  }

  int FifoDepthToPosition(int depth)
  {
    return NormalizeFifoDepth(depth) / FifoStep;
  }

  int NormalizeFifoDepth(int depth)
  {
    // As with the ASIO block, round upward rather than reducing buffering.
    int position = (depth + FifoStep - 1) / FifoStep;
    return FifoDepthFromPosition(position);
  }
}
