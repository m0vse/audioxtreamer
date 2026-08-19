#pragma once

#include "resource.h"

class CLogDlg : public CPropertyPage
{
public:
  CLogDlg();

#ifdef AFX_DESIGN_TIME
  enum { IDD = IDD_DLG_LOG };
#endif

protected:
  BOOL OnInitDialog() override;
  BOOL OnSetActive() override;
  void DoDataExchange(CDataExchange* pDX) override;
  DECLARE_MESSAGE_MAP()

  afx_msg void OnTimer(UINT_PTR nIDEvent);
  afx_msg void OnDestroy();
  afx_msg void OnCopy();
  afx_msg void OnClear();

private:
  void Refresh(bool force);

  CEdit mLogText;
  ULONGLONG mLastFileSize;
  FILETIME mLastWriteTime;
};
