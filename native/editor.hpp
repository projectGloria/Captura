#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <gdiplus.h>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <vector>

#include <windowsx.h>
using namespace Gdiplus;
using std::make_unique;
using std::unique_ptr;
using std::vector;
using std::wstring;
namespace fs = std::filesystem;

enum Mode {
  RectangleMode,
  WindowMode,
  MonitorMode,
  FreeMode,
  PickerMode,
  OcrMode
};
enum Tool {
  Move,
  PenTool,
  Highlighter,
  Line,
  Arrow,
  Box,
  Filled,
  Oval,
  Text,
  Number,
  Mosaic
};
struct Mark {
  Tool tool = Move;
  vector<Point> points;
  Color color = Color(255, 225, 60, 98);
  int width = 4;
  wstring text;
};
struct Document {
  Rect selection;
  vector<Point> outline;
  vector<Mark> marks;
};
inline Rect bounds(Point a, Point b) {
  return Rect(std::min(a.X, b.X), std::min(a.Y, b.Y), std::abs(a.X - b.X),
              std::abs(a.Y - b.Y));
}
inline Point offset(Point p, int dx, int dy) {
  return Point(p.X + dx, p.Y + dy);
}
inline bool withinRadius(Point a, Point b, int radius) {
  return std::abs(a.X - b.X) <= radius && std::abs(a.Y - b.Y) <= radius;
}
inline Point center(Rect r) {
  return Point(r.X + r.Width / 2, r.Y + r.Height / 2);
}
inline void solid(Graphics &g, Rect r, Color c) {
  SolidBrush b(c);
  g.FillRectangle(&b, r);
}
inline void label(Graphics &g, const wstring &s, Rect r, float size, Color c,
                  bool centered = true, const wchar_t *face = L"Segoe UI") {
  FontFamily f(face);
  Font font(&f, size, FontStyleRegular, UnitPixel);
  SolidBrush b(c);
  StringFormat fmt;
  fmt.SetAlignment(centered ? StringAlignmentCenter : StringAlignmentNear);
  fmt.SetLineAlignment(StringAlignmentCenter);
  fmt.SetFormatFlags(StringFormatFlagsNoWrap);
  RectF area((REAL)r.X, (REAL)r.Y, (REAL)r.Width, (REAL)r.Height);
  g.DrawString(s.c_str(), (INT)s.size(), &font, area, &fmt, &b);
}

// A capture session owns its pixel buffers and reuses them for every repaint.
struct Surface {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  DWORD *pixels = nullptr;
  Surface() = default;
  Surface(const Surface &) = delete;
  Surface &operator=(const Surface &) = delete;
  bool allocate(HDC screen, const BITMAPINFO &info) {
    dc = CreateCompatibleDC(screen);
    void *data = nullptr;
    bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    if (!dc || !bitmap)
      return false;
    previous = SelectObject(dc, bitmap);
    pixels = static_cast<DWORD *>(data);
    return previous && previous != HGDI_ERROR;
  }
  ~Surface() {
    if (previous && previous != HGDI_ERROR)
      SelectObject(dc, previous);
    if (bitmap)
      DeleteObject(bitmap);
    if (dc)
      DeleteDC(dc);
  }
};
struct Desktop {
  Rect area;
  Surface original, dimmed, canvas;
  unique_ptr<Bitmap> frame;
  vector<Rect> windows;
  bool freeze() {
    area = Rect(GetSystemMetrics(SM_XVIRTUALSCREEN),
                GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN),
                GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (area.Width <= 0 || area.Height <= 0)
      return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biWidth = area.Width;
    info.bmiHeader.biHeight = -area.Height;
    HDC screen = GetDC(nullptr);
    if (!screen)
      return false;
    bool ok = original.allocate(screen, info) &&
              dimmed.allocate(screen, info) && canvas.allocate(screen, info) &&
              BitBlt(original.dc, 0, 0, area.Width, area.Height, screen, area.X,
                     area.Y, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    if (!ok)
      return false;
    GdiFlush();
    const size_t count = (size_t)area.Width * area.Height;
    for (size_t i = 0; i < count; ++i)
      dimmed.pixels[i] = ((original.pixels[i] & 0x00fefefe) >> 1) + 0x060409;
    frame = make_unique<Bitmap>(area.Width, area.Height, area.Width * 4,
                                PixelFormat32bppRGB,
                                reinterpret_cast<BYTE *>(original.pixels));
    if (frame->GetLastStatus() != Ok)
      return false;
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
          if (!IsWindowVisible(h) || IsIconic(h) ||
              GetWindow(h, GW_OWNER) &&
                  (GetWindowLongPtr(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
            return TRUE;
          DWORD cloaked = 0;
          DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
          if (cloaked)
            return TRUE;
          RECT r{};
          if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r,
                                           sizeof(r))))
            GetWindowRect(h, &r);
          auto &d = *reinterpret_cast<Desktop *>(l);
          Rect b(r.left - d.area.X, r.top - d.area.Y, r.right - r.left,
                 r.bottom - r.top);
          b.Intersect(Rect(0, 0, d.area.Width, d.area.Height));
          if (!b.IsEmptyArea())
            d.windows.push_back(b);
          return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
    return true;
  }
  Rect monitor(Point p, bool work = false) const {
    MONITORINFO i{sizeof(i)};
    POINT q{p.X + area.X, p.Y + area.Y};
    GetMonitorInfoW(MonitorFromPoint(q, MONITOR_DEFAULTTONEAREST), &i);
    auto r = work ? i.rcWork : i.rcMonitor;
    return Rect(r.left - area.X, r.top - area.Y, r.right - r.left,
                r.bottom - r.top);
  }
  Color sample(Point p) const {
    p.X = std::clamp(p.X, 0, area.Width - 1);
    p.Y = std::clamp(p.Y, 0, area.Height - 1);
    auto b = reinterpret_cast<const BYTE *>(original.pixels +
                                            (size_t)p.Y * area.Width + p.X);
    return Color(255, b[2], b[1], b[0]);
  }
};
inline void clipSelection(Graphics &g, const Document &d) {
  g.SetClip(d.selection, CombineModeIntersect);
  if (d.outline.size() > 2) {
    GraphicsPath path;
    path.AddPolygon(d.outline.data(), (int)d.outline.size());
    g.SetClip(&path, CombineModeIntersect);
  }
}
inline void drawMark(Graphics &g, const Mark &m, const Desktop &desktop) {
  if (m.points.empty())
    return;
  Point a = m.points.front(), b = m.points.back();
  Rect r = bounds(a, b);
  Color c = m.color;
  if (m.tool == Highlighter)
    c = Color(105, c.GetR(), c.GetG(), c.GetB());
  int width = m.tool == Highlighter ? m.width * 4 : m.width;
  Pen pen(c, (REAL)width);
  pen.SetLineJoin(LineJoinRound);
  pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
  SolidBrush brush(c);
  switch (m.tool) {
  case PenTool:
  case Highlighter:
    if (m.points.size() > 1)
      g.DrawLines(&pen, m.points.data(), (int)m.points.size());
    else
      g.FillEllipse(&brush, a.X - width / 2, a.Y - width / 2, width, width);
    break;
  case Arrow: {
    AdjustableArrowCap cap(4, 5, true);
    pen.SetCustomEndCap(&cap);
    g.DrawLine(&pen, a, b);
    break;
  }
  case Line:
    g.DrawLine(&pen, a, b);
    break;
  case Box:
    g.DrawRectangle(&pen, r);
    break;
  case Filled:
    g.FillRectangle(&brush, r);
    break;
  case Oval:
    g.DrawEllipse(&pen, r);
    break;
  case Number: {
    int rad = 10 + width;
    g.FillEllipse(&brush, a.X - rad, a.Y - rad, rad * 2, rad * 2);
    label(g, m.text, Rect(a.X - rad, a.Y - rad, rad * 2, rad * 2),
          (float)rad * 1.2f, Color::White);
    break;
  }
  case Text: {
    FontFamily family(L"Segoe UI");
    Font font(&family, (REAL)(14 + width * 2), FontStyleRegular, UnitPixel);
    g.DrawString(m.text.c_str(), -1, &font, PointF((REAL)a.X, (REAL)a.Y),
                 &brush);
    break;
  }
  case Mosaic: {
    int step = std::max(8, width * 3);
    for (int y = r.Y; y < r.GetBottom(); y += step)
      for (int x = r.X; x < r.GetRight(); x += step)
        solid(g,
              Rect(x, y, std::min(step, r.GetRight() - x),
                   std::min(step, r.GetBottom() - y)),
              desktop.sample(Point(x + step / 2, y + step / 2)));
    break;
  }
  default:
    break;
  }
}
inline void drawSelected(Graphics &g, const Document &d, const Desktop &desktop,
                         bool drawFrame = true) {
  auto state = g.Save();
  clipSelection(g, d);
  if (drawFrame)
    g.DrawImage(desktop.frame.get(), d.selection, d.selection.X, d.selection.Y,
                d.selection.Width, d.selection.Height, UnitPixel);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  for (auto &m : d.marks)
    drawMark(g, m, desktop);
  g.Restore(state);
}
inline vector<BYTE> renderPixels(const Document &d, const Desktop &desktop) {
  const auto r = d.selection;
  if (r.IsEmptyArea())
    return {};
  Bitmap image(r.Width, r.Height, PixelFormat32bppARGB);
  {
    Graphics g(&image);
    g.Clear(Color(0, 0, 0, 0));
    g.TranslateTransform((REAL)-r.X, (REAL)-r.Y);
    drawSelected(g, d, desktop);
  }
  BitmapData lock{};
  Rect local(0, 0, r.Width, r.Height);
  if (image.LockBits(&local, ImageLockModeRead, PixelFormat32bppARGB, &lock) !=
      Ok)
    return {};
  vector<BYTE> out((size_t)r.Width * r.Height * 4);
  for (int y = 0; y < r.Height; ++y) {
    auto src = (BYTE *)lock.Scan0 + y * lock.Stride;
    auto dst = out.data() + (size_t)y * r.Width * 4;
    for (int x = 0; x < r.Width; ++x, src += 4, dst += 4) {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = src[3];
    }
  }
  image.UnlockBits(&lock);
  return out;
}
inline std::array<Point, 8> handles(Rect r) {
  int l = r.X, t = r.Y, b = r.GetBottom(), right = r.GetRight(),
      x = l + r.Width / 2, y = t + r.Height / 2;
  return {Point(l, t),     Point(x, t), Point(right, t), Point(right, y),
          Point(right, b), Point(x, b), Point(l, b),     Point(l, y)};
}
inline void changeBounds(Document &d, const Document &before, Rect next,
                         bool moving) {
  Rect r = before.selection;
  d = before;
  d.selection = next;
  auto transform = [&](Point p) {
    return Point(next.X + MulDiv(p.X - r.X, next.Width, r.Width),
                 next.Y + MulDiv(p.Y - r.Y, next.Height, r.Height));
  };
  for (auto &p : d.outline)
    p = transform(p);
  if (moving)
    for (auto &m : d.marks)
      for (auto &p : m.points)
        p = offset(p, next.X - r.X, next.Y - r.Y);
}
