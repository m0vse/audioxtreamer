#include "stdafx.h"
#include "AppLog.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace
{
  const wchar_t* const LogMutexName = L"Local\\AudioXtreamerLogMutex";
  const unsigned long long MaxLogSize = 8ULL * 1024ULL * 1024ULL;

  class LogLock
  {
  public:
    LogLock()
      : mHandle(CreateMutexW(nullptr, FALSE, LogMutexName))
      , mLocked(false)
    {
      if (mHandle != nullptr)
      {
        const DWORD result = WaitForSingleObject(mHandle, 2000);
        mLocked = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
      }
    }

    ~LogLock()
    {
      if (mLocked)
        ReleaseMutex(mHandle);
      if (mHandle != nullptr)
        CloseHandle(mHandle);
    }

    bool Locked() const { return mLocked; }

  private:
    HANDLE mHandle;
    bool mLocked;
  };

  std::wstring BuildLogPath()
  {
    wchar_t localAppData[MAX_PATH] = { 0 };
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
      length = GetTempPathW(MAX_PATH, localAppData);
      if (length == 0 || length >= MAX_PATH)
        return L"AudioXtreamer.log";
    }

    std::wstring directory(localAppData);
    if (!directory.empty() && directory.back() != L'\\')
      directory += L'\\';
    directory += L"AudioXtreamer";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory + L"\\AudioXtreamer.log";
  }

  void WriteLine(const char* level, const char* component, const char* format, va_list args)
  {
    char message[2048] = { 0 };
    _vsnprintf_s(message, _countof(message), _TRUNCATE, format, args);

    SYSTEMTIME now;
    GetLocalTime(&now);

    char line[2304] = { 0 };
    _snprintf_s(line, _countof(line), _TRUNCATE,
      "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] [%s] [pid:%lu tid:%lu] %s\r\n",
      now.wYear, now.wMonth, now.wDay,
      now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
      level, component != nullptr ? component : "General",
      GetCurrentProcessId(), GetCurrentThreadId(), message);

    LogLock lock;
    if (!lock.Locked())
      return;

    const std::wstring path = BuildLogPath();
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
      return;

    LARGE_INTEGER size = { 0 };
    if (GetFileSizeEx(file, &size) && static_cast<unsigned long long>(size.QuadPart) > MaxLogSize)
    {
      CloseHandle(file);
      file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    if (file != INVALID_HANDLE_VALUE)
    {
      DWORD written = 0;
      WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
      CloseHandle(file);
    }
  }

  void Write(const char* level, const char* component, const char* format, va_list args)
  {
    WriteLine(level, component, format, args);
  }
}

void AppLog::Info(const char* component, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  Write("INFO", component, format, args);
  va_end(args);
}

void AppLog::Warning(const char* component, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  Write("WARN", component, format, args);
  va_end(args);
}

void AppLog::Error(const char* component, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  Write("ERROR", component, format, args);
  va_end(args);
}

std::wstring AppLog::FilePath()
{
  return BuildLogPath();
}

bool AppLog::ReadTail(std::string& text, unsigned long maxBytes)
{
  text.clear();
  const std::wstring path = BuildLogPath();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return GetLastError() == ERROR_FILE_NOT_FOUND;

  LARGE_INTEGER size = { 0 };
  if (!GetFileSizeEx(file, &size))
  {
    CloseHandle(file);
    return false;
  }

  const unsigned long long fileSize = static_cast<unsigned long long>(size.QuadPart);
  const DWORD bytesToRead = static_cast<DWORD>(fileSize < maxBytes ? fileSize : maxBytes);
  const unsigned long long offset = fileSize - bytesToRead;
  LARGE_INTEGER position;
  position.QuadPart = offset;
  SetFilePointerEx(file, position, nullptr, FILE_BEGIN);

  std::vector<char> buffer(bytesToRead + 1, 0);
  DWORD bytesRead = 0;
  const BOOL result = bytesToRead == 0 || ReadFile(file, buffer.data(), bytesToRead, &bytesRead, nullptr);
  CloseHandle(file);
  if (!result)
    return false;

  text.assign(buffer.data(), bytesRead);
  if (offset > 0)
  {
    const size_t firstLine = text.find('\n');
    if (firstLine != std::string::npos)
      text.erase(0, firstLine + 1);
    text.insert(0, "[Earlier entries omitted from this view]\r\n");
  }
  return true;
}

bool AppLog::Clear()
{
  LogLock lock;
  if (!lock.Locked())
    return false;

  const std::wstring path = BuildLogPath();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  CloseHandle(file);
  return true;
}
