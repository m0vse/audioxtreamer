#pragma once

#include <afxwin.h>

enum class DeviceStatusState
{
  NotConnected,
  ConnectedIdle,
  Playing
};

class DeviceStatusCtrl : public CStatic
{
public:
  DeviceStatusCtrl()
    : mState(DeviceStatusState::NotConnected)
  {
  }

  void SetStatus(DeviceStatusState state)
  {
    if (mState == state && GetWindowTextLength() != 0)
      return;

    mState = state;
    switch (mState)
    {
    case DeviceStatusState::NotConnected:
      SetWindowText(_T("FPGA not connected"));
      break;
    case DeviceStatusState::ConnectedIdle:
      SetWindowText(_T("FPGA connected - idle"));
      break;
    case DeviceStatusState::Playing:
      SetWindowText(_T("Playing"));
      break;
    }
    Invalidate(FALSE);
  }

  void DrawItem(LPDRAWITEMSTRUCT drawItem) override
  {
    CDC dc;
    dc.Attach(drawItem->hDC);

    CRect bounds(drawItem->rcItem);
    dc.FillSolidRect(bounds, GetSysColor(COLOR_3DFACE));
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(GetSysColor(COLOR_BTNTEXT));

    CFont* font = GetParent() ? GetParent()->GetFont() : nullptr;
    CFont* previousFont = font ? dc.SelectObject(font) : nullptr;

    const COLORREF activeColors[] = {
      RGB(210, 45, 45),
      RGB(230, 165, 25),
      RGB(35, 170, 75)
    };
    const COLORREF inactiveColors[] = {
      RGB(125, 85, 85),
      RGB(130, 115, 75),
      RGB(75, 115, 85)
    };

    const int lampSize = min(11, max(7, bounds.Height() - 4));
    const int lampTop = bounds.top + (bounds.Height() - lampSize) / 2;
    CPen outline(PS_SOLID, 1, RGB(70, 70, 70));
    CPen* previousPen = dc.SelectObject(&outline);

    for (int lamp = 0; lamp < 3; ++lamp)
    {
      const bool active = lamp == static_cast<int>(mState);
      CBrush fill(active ? activeColors[lamp] : inactiveColors[lamp]);
      CBrush* previousBrush = dc.SelectObject(&fill);
      const int left = bounds.left + 2 + lamp * (lampSize + 3);
      dc.Ellipse(left, lampTop, left + lampSize, lampTop + lampSize);
      dc.SelectObject(previousBrush);
    }

    dc.SelectObject(previousPen);

    CString text;
    GetWindowText(text);
    CRect textRect(bounds);
    textRect.left += 2 + 3 * (lampSize + 3) + 4;
    dc.DrawText(text, textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (previousFont)
      dc.SelectObject(previousFont);
    dc.Detach();
  }

private:
  DeviceStatusState mState;
};
