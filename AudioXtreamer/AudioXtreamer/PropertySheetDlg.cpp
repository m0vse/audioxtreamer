#include "stdafx.h"
#include "PropertySheetDlg.h"
#include "MainFrame.h"
#include "AudioXtreamer.h"



class CAboutDlg : public CDialog
{
public:
  CAboutDlg();

  // Dialog Data
#ifdef AFX_DESIGN_TIME
  enum { IDD = IDD_ABOUTBOX };
#endif

protected:
  virtual void DoDataExchange(CDataExchange * DX);    // DDX/DDV support

// Implementation
protected:

public:

};


CAboutDlg::CAboutDlg() : CDialog(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* DX)
{
  CDialog::DoDataExchange(DX);
}


// PropertySheet dialog

#include "SettingsDlg.cpp"
#include "AudioXtreamerDlg.cpp"
#include "LogDlg.cpp"


PropertySheetDlg::PropertySheetDlg(UsbDevice &usbdev, MainFrame * parent)
: CPropertySheet(szNameApp, parent)
, mDevice(usbdev)
, pp1(new ASIOSettingsDlg( usbdev, theSettings))
, pp2(new CAudioXtreamerDlg(usbdev, theSettings))
, pp3(new CLogDlg())
{
  AddPage(pp1);
  AddPage(pp2);
  AddPage(pp3);
}

PropertySheetDlg::~PropertySheetDlg()
{
  RemovePage(pp3);
  RemovePage(pp2);
  RemovePage(pp1);
  delete pp3;
  delete pp2;
  delete pp1;
}

BOOL PropertySheetDlg::OnInitDialog()
{
  BOOL bResult = CPropertySheet::OnInitDialog();

  //Menu.LoadMenu(IDR_MENU);
  mHicon = AfxGetApp()->LoadIcon(IDI_AXG);

  CMenu * sysMenu = GetSystemMenu(FALSE);
  sysMenu->InsertMenu(1, MF_BYPOSITION, IDM_ABOUTBOX, _T("&About"));
  sysMenu->InsertMenu(3, MF_BYPOSITION, 1234, _T("&Exit"));
  SetIcon(mHicon, TRUE);
  SetIcon(mHicon, FALSE);

  CWnd* okButton = GetDlgItem(IDOK);
  if (okButton)
  {
    CRect buttonRect;
    CRect clientRect;
    okButton->GetWindowRect(&buttonRect);
    ScreenToClient(&buttonRect);
    GetClientRect(&clientRect);
    CRect statusRect(8, buttonRect.top, buttonRect.left - 8, buttonRect.bottom);
    if (statusRect.Width() >= 120)
      mDeviceStatus.Create(_T(""), WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
        statusRect, this, 0x5001);
  }

  UpdateDeviceStatus();
  SetTimer(300, 100, nullptr);

  return bResult;
}


BEGIN_MESSAGE_MAP(PropertySheetDlg, CPropertySheet)
  ON_WM_DESTROY()
  ON_WM_TIMER()
  ON_WM_SYSCOMMAND()
  ON_COMMAND(ID_APPLY_NOW, &PropertySheetDlg::OnApplyNow)
END_MESSAGE_MAP()


void PropertySheetDlg::OnDestroy()
{
  KillTimer(300);
  CPropertySheet::OnDestroy();
}

void PropertySheetDlg::OnTimer(UINT_PTR nIDEvent)
{
  if (nIDEvent == 300)
    UpdateDeviceStatus();
  CPropertySheet::OnTimer(nIDEvent);
}

void PropertySheetDlg::UpdateDeviceStatus()
{
  if (!mDeviceStatus.GetSafeHwnd())
    return;

  DeviceStatusState state = DeviceStatusState::NotConnected;
  if (mDevice.IsPresent())
  {
    state = mDevice.IsRunning() && theApp.IsClientActive()
      ? DeviceStatusState::Playing
      : DeviceStatusState::ConnectedIdle;
  }
  mDeviceStatus.SetStatus(state);
}

void PropertySheetDlg::OnApplyNow()
{
  Default();

  // parent window of property sheet
  if (m_pParentWnd)
    ((MainFrame*)m_pParentWnd)->SaveSettings();


}

void PropertySheetDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
  if ((nID & 0xFFF0) == IDM_ABOUTBOX)
  {
    CAboutDlg dlgAbout;
    dlgAbout.DoModal();
  }
  else
  {
    CPropertySheet::OnSysCommand(nID, lParam);
  }
}

bool PropertySheetDlg::ManualControls()
{
  return GetActiveIndex() == 1;
}
