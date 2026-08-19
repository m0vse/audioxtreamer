#pragma once

#include "SettingsDlg.h"
#include "AudioXtreamerDlg.h"
#include "LogDlg.h"
#include "DeviceStatusCtrl.h"

class MainFrame;
class PropertySheetDlg : public CPropertySheet
{
public:

  PropertySheetDlg(UsbDevice &usbdev, MainFrame*parent);
  ~PropertySheetDlg();
  bool ManualControls();
  virtual BOOL OnInitDialog();
  void UpdateDeviceStatus();
 
protected:
  UsbDevice &mDevice;

  CPropertyPage* pp1;
  CPropertyPage* pp2;
  CPropertyPage* pp3;

  CMenu Menu;
  HICON mHicon;
  DeviceStatusCtrl mDeviceStatus;

public:
  DECLARE_MESSAGE_MAP()
  afx_msg void OnDestroy();
  afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
  afx_msg void OnApplyNow();
  afx_msg void OnTimer(UINT_PTR nIDEvent);
};
