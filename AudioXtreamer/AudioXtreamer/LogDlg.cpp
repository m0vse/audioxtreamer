#include "stdafx.h"
#include "LogDlg.h"
#include "AppLog.h"

CLogDlg::CLogDlg()
  : CPropertyPage(IDD_DLG_LOG)
  , mLastFileSize(static_cast<ULONGLONG>(-1))
  , mLastWriteTime({ 0, 0 })
{
}

BEGIN_MESSAGE_MAP(CLogDlg, CPropertyPage)
  ON_WM_TIMER()
  ON_WM_DESTROY()
  ON_BN_CLICKED(IDC_LOG_COPY, &CLogDlg::OnCopy)
  ON_BN_CLICKED(IDC_LOG_CLEAR, &CLogDlg::OnClear)
END_MESSAGE_MAP()

void CLogDlg::DoDataExchange(CDataExchange* pDX)
{
  CPropertyPage::DoDataExchange(pDX);
  DDX_Control(pDX, IDC_LOG_TEXT, mLogText);
}

BOOL CLogDlg::OnInitDialog()
{
  CPropertyPage::OnInitDialog();
  Refresh(true);
  SetTimer(300, 500, nullptr);
  return TRUE;
}

BOOL CLogDlg::OnSetActive()
{
  Refresh(true);
  return CPropertyPage::OnSetActive();
}

void CLogDlg::OnDestroy()
{
  KillTimer(300);
  CPropertyPage::OnDestroy();
}

void CLogDlg::OnTimer(UINT_PTR nIDEvent)
{
  if (nIDEvent == 300)
    Refresh(false);
  CPropertyPage::OnTimer(nIDEvent);
}

void CLogDlg::Refresh(bool force)
{
  WIN32_FILE_ATTRIBUTE_DATA attributes = { 0 };
  const std::wstring path = AppLog::FilePath();
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
  {
    if (force)
      mLogText.SetWindowText(_T("No log entries yet."));
    return;
  }

  ULARGE_INTEGER size;
  size.HighPart = attributes.nFileSizeHigh;
  size.LowPart = attributes.nFileSizeLow;
  if (!force && size.QuadPart == mLastFileSize &&
      CompareFileTime(&attributes.ftLastWriteTime, &mLastWriteTime) == 0)
    return;

  std::string bytes;
  if (!AppLog::ReadTail(bytes))
    return;

  int required = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
    static_cast<int>(bytes.size()), nullptr, 0);
  CString text;
  if (required > 0)
  {
    wchar_t* buffer = text.GetBuffer(required);
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), buffer, required);
    text.ReleaseBuffer(required);
  }

  mLogText.SetWindowText(text);
  mLogText.SetSel(-1, -1);
  mLogText.LineScroll(mLogText.GetLineCount());
  mLastFileSize = size.QuadPart;
  mLastWriteTime = attributes.ftLastWriteTime;
}

void CLogDlg::OnCopy()
{
  mLogText.SetSel(0, -1);
  mLogText.Copy();
  mLogText.SetSel(-1, -1);
}

void CLogDlg::OnClear()
{
  if (AppLog::Clear())
  {
    AppLog::Info("UI", "Log cleared by user");
    Refresh(true);
  }
  else
  {
    AfxMessageBox(_T("The log could not be cleared."), MB_ICONWARNING | MB_OK);
  }
}
