#pragma once

#include <string>

namespace AppLog
{
  void Info(const char* component, const char* format, ...);
  void Warning(const char* component, const char* format, ...);
  void Error(const char* component, const char* format, ...);

  std::wstring FilePath();
  bool ReadTail(std::string& text, unsigned long maxBytes = 512 * 1024);
  bool Clear();
}
