#include "platform.hpp"
#include <shellscalingapi.h>
#include <thread>
#pragma comment(lib, "shcore.lib")

constexpr UINT TrayMessage = WM_APP + 1, Activate = WM_APP + 2,
               OcrDone = WM_APP + 3;
enum Command {
  ModeBase = 100,
  ToolBase = 120,
  Undo = 150,
  Redo,
  PaintColor,
  Weight,
  Save,
  Copy,
  Cancel,
  Settings,
  Recognize
};
struct Button {
  Rect r;
  int id;
  wstring caption, tip;
  bool glyph;
};
enum Drag { Idle, Selecting, Resizing, Moving, Drawing, MovingMark };
struct Session {
  Desktop desktop;
  Document doc, before;
  vector<Document> undo, redo;
  Mode mode = RectangleMode;
  Tool tool = Move;
  Drag drag = Idle;
  Point mouse, start, anchor;
  int handle = -1, chosen = -1, stroke = 4;
  Color ink = Color(255, 237, 68, 100);
  vector<Button> buttons;
  bool selected = false;
  wstring saved;
  void checkpoint() {
    if (undo.size() >= 50)
      undo.erase(undo.begin());
    undo.push_back(before);
    redo.clear();
    saved.clear();
  }
};
static unique_ptr<Session> s;
static HWND host = nullptr, overlay = nullptr;
static Preferences prefs;
static NOTIFYICONDATAW tray{};
static bool registered = false, modal = false, recognizing = false;
static UINT taskbarCreated;
static HINSTANCE instance;
static const wchar_t *hostClass = L"projectGloria.Captura.Controller";
static const wchar_t *overlayClass = L"projectGloria.Captura.Overlay";
static void refresh() {
  if (overlay)
    InvalidateRect(overlay, nullptr, FALSE);
}
static void notify(const wchar_t *title, const wstring &message) {
  tray.uFlags = NIF_INFO;
  wcscpy_s(tray.szInfoTitle, title);
  wcsncpy_s(tray.szInfo, message.c_str(), _TRUNCATE);
  tray.dwInfoFlags = NIIF_INFO;
  Shell_NotifyIconW(NIM_MODIFY, &tray);
}
static bool registerKey(WORD key) {
  BYTE mods = HIBYTE(key);
  UINT flags = MOD_NOREPEAT;
  if (mods & HOTKEYF_CONTROL)
    flags |= MOD_CONTROL;
  if (mods & HOTKEYF_ALT)
    flags |= MOD_ALT;
  if (mods & HOTKEYF_SHIFT)
    flags |= MOD_SHIFT;
  return LOBYTE(key) && RegisterHotKey(host, 1, flags, LOBYTE(key));
}
static INT_PTR CALLBACK settingsProc(HWND h, UINT msg, WPARAM wp, LPARAM) {
  if (msg == WM_INITDIALOG) {
    SetDlgItemTextW(h, 101, prefs.dir.c_str());
    SendDlgItemMessageW(h, 103, HKM_SETHOTKEY, prefs.key, 0);
    return TRUE;
  }
  if (msg == WM_COMMAND)
    switch (LOWORD(wp)) {
    case 102: {
      auto d = folderPicker(h, field(h, 101));
      if (!d.empty())
        SetDlgItemTextW(h, 101, d.c_str());
      return TRUE;
    }
    case IDOK: {
      wstring dir = field(h, 101);
      WORD key = (WORD)SendDlgItemMessageW(h, 103, HKM_GETHOTKEY, 0, 0);
      if (dir.empty() || !fs::path(dir).is_absolute()) {
        MessageBoxW(h, L"Choose an absolute folder path.", L"Captura", MB_OK);
        return TRUE;
      }
      bool change = key != prefs.key || !registered;
      if (change) {
        if (registered)
          UnregisterHotKey(host, 1);
        if (!registerKey(key)) {
          registered = registerKey(prefs.key);
          MessageBoxW(
              h, L"That shortcut is unavailable. Choose another combination.",
              L"Captura", MB_OK);
          return TRUE;
        }
        registered = true;
      }
      auto old = prefs;
      prefs.key = key;
      prefs.dir = dir;
      if (!prefs.save()) {
        prefs = old;
        if (change) {
          UnregisterHotKey(host, 1);
          registered = registerKey(old.key);
        }
        MessageBoxW(h, L"Windows could not save your settings.", L"Captura",
                    MB_OK);
        return TRUE;
      }
      EndDialog(h, IDOK);
      return TRUE;
    }
    case IDCANCEL:
      EndDialog(h, IDCANCEL);
      return TRUE;
    }
  if (msg == WM_CLOSE) {
    EndDialog(h, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}
static void settings() {
  modal = true;
  DialogBoxParamW(instance, MAKEINTRESOURCEW(201), overlay ? overlay : host,
                  settingsProc, 0);
  modal = false;
  refresh();
}
static void finish() {
  if (overlay) {
    HWND old = overlay;
    overlay = nullptr;
    DestroyWindow(old);
  }
  s.reset();
}
static void changeMode(Mode m) {
  auto &a = *s;
  a.mode = m;
  a.doc = {};
  a.undo.clear();
  a.redo.clear();
  a.saved.clear();
  a.selected = false;
  a.chosen = -1;
  a.tool = Move;
  a.drag = Idle;
  if (m == MonitorMode)
    a.doc.selection = a.desktop.monitor(a.mouse);
  if (m == WindowMode)
    for (auto r : a.desktop.windows)
      if (r.Contains(a.mouse)) {
        a.doc.selection = r;
        break;
      }
  refresh();
}
static wstring colorString(Color c) {
  wchar_t buf[16];
  swprintf_s(buf, L"#%02X%02X%02X", c.GetR(), c.GetG(), c.GetB());
  return buf;
}
static int dpiMetric(int n, Point p) {
  POINT pt{p.X + s->desktop.area.X, p.Y + s->desktop.area.Y};
  UINT x = 96, y = 96;
  GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST),
                   MDT_EFFECTIVE_DPI, &x, &y);
  return MulDiv(n, x, 96);
}
static const wchar_t *modes[] = {L"Rectangle", L"Window", L"Full screen",
                                 L"Free form", L"Color",  L"Extract text"};
static const wchar_t *toolNames[] = {L"Select / move",
                                     L"Pen",
                                     L"Highlighter",
                                     L"Line",
                                     L"Arrow",
                                     L"Rectangle outline",
                                     L"Filled rectangle",
                                     L"Ellipse",
                                     L"Text",
                                     L"Numbered marker",
                                     L"Pixelate"};
static const wchar_t *icons[] = {L"\uE7C9", L"\uE70F", L"\uE7E6", L"\uE738",
                                 L"\uE72A", L"\uE739", L"\uE73B", L"\uEA3A",
                                 L"\uE8D2", L"\uE8BC", L"\uECA5"};
static void layout() {
  auto &a = *s;
  a.buttons.clear();
  auto monitor =
      a.desktop.monitor(a.selected ? center(a.doc.selection) : a.anchor);
  Point reference = center(monitor);
  auto D = [&](int n) { return dpiMetric(n, reference); };
  int gap = D(4), margin = D(10), line = D(32);
  const int widths[] = {82, 70, 82, 78, 55, 87};
  int total = D(32) + gap * 6;
  for (int w : widths)
    total += D(w);
  int x = monitor.X + (monitor.Width - total) / 2, y = monitor.Y + margin;
  for (int i = 0; i < 6; ++i) {
    int w = D(widths[i]);
    a.buttons.push_back(
        {Rect(x, y, w, line), ModeBase + i, modes[i], modes[i], false});
    x += w + gap;
  }
  a.buttons.push_back(
      {Rect(x, y, line, line), Settings, L"\uE713", L"Settings", true});
  if (!a.selected || a.drag != Idle)
    return;
  vector<Button> items;
  auto add = [&](int id, const wchar_t *icon, const wchar_t *tip) {
    items.push_back({{}, id, icon, tip, true});
  };
  for (int i = 0; i < 11; ++i)
    add(ToolBase + i, icons[i], toolNames[i]);
  add(PaintColor, L"\uE790", L"Ink color");
  add(Weight, L"\uE8A4", L"Stroke width");
  add(Undo, L"\uE7A7", L"Undo (Ctrl+Z)");
  add(Redo, L"\uE7A6", L"Redo (Ctrl+Y)");
  add(Recognize, L"\uE8C1", L"Extract text");
  add(Copy, L"\uE8C8", L"Copy and save (Ctrl+C)");
  add(Save, L"\uE74E", L"Save (Enter)");
  add(Cancel, L"\uE711", L"Discard (Esc)");
  int size = D(34), pitch = size + gap;
  int capacity = std::max(1, (monitor.Width - 2 * margin) / pitch);
  int cols = std::min({(int)items.size(), capacity,
                       std::max(7, a.doc.selection.Width / pitch)});
  int rows = ((int)items.size() + cols - 1) / cols, ht = rows * pitch - gap;
  auto r = a.doc.selection;
  int left = std::clamp(
      r.X, monitor.X + margin,
      std::max(monitor.X + margin, monitor.GetRight() - margin - cols * pitch));
  int top = r.GetBottom() + D(10);
  if (top + ht > monitor.GetBottom() - margin)
    top = r.Y - D(10) - ht;
  top = std::clamp(top, monitor.Y + line + 2 * margin,
                   std::max(monitor.Y + line + 2 * margin,
                            monitor.GetBottom() - margin - ht));
  for (int i = 0; i < (int)items.size(); ++i) {
    items[i].r =
        Rect(left + (i % cols) * pitch, top + (i / cols) * pitch, size, size);
    a.buttons.push_back(items[i]);
  }
}
static void pill(Graphics &g, Rect r, Color c, int radius) {
  int d = std::max(2, std::min({radius * 2, r.Width, r.Height}));
  GraphicsPath p;
  p.AddArc(r.X, r.Y, d, d, 180, 90);
  p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
  p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
  p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
  p.CloseFigure();
  SolidBrush b(c);
  g.FillPath(&b, &p);
}
static bool drawToolSymbol(Graphics &g, const Button &button) {
  int tool = button.id - ToolBase;
  if (tool < Line || tool > Mosaic)
    return false;
  auto r = button.r;
  int size = std::max(8, r.Height * 42 / 100);
  Point c = center(r);
  Rect icon(c.X - size / 2, c.Y - size / 2, size, size);
  Pen outline(Color::White, std::max(1.5f, size / 8.f));
  SolidBrush white(Color::White);
  switch (tool) {
  case Line:
  case Arrow:
    g.DrawLine(&outline, icon.X, icon.GetBottom(), icon.GetRight(), icon.Y);
    if (tool == Arrow) {
      g.DrawLine(&outline, icon.GetRight(), icon.Y, icon.GetRight(), c.Y);
      g.DrawLine(&outline, c.X, icon.Y, icon.GetRight(), icon.Y);
    }
    break;
  case Box:
    g.DrawRectangle(&outline, icon);
    break;
  case Filled:
    g.FillRectangle(&white, icon);
    break;
  case Oval:
  case Number:
    g.DrawEllipse(&outline, icon);
    if (tool == Number)
      label(g, L"1", r, (float)size * .85f, Color::White);
    break;
  case Text:
    label(g, L"T", r, (float)size * 1.4f, Color::White);
    break;
  case Mosaic: {
    int gap = std::max(2, size / 4);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        g.FillRectangle(&white, icon.X + x * gap, icon.Y + y * gap, gap - 1,
                        gap - 1);
    break;
  }
  default:
    return false;
  }
  return true;
}
static void paint(HWND h) {
  PAINTSTRUCT ps{};
  HDC dc = BeginPaint(h, &ps);
  if (!s) {
    EndPaint(h, &ps);
    return;
  }
  auto &a = *s;
  auto &d = a.desktop;
  int W = d.area.Width, H = d.area.Height;
  HDC back = d.canvas.dc;
  bool pick = a.mode == PickerMode;
  Rect r = a.doc.selection;
  BitBlt(back, 0, 0, W, H, pick ? d.original.dc : d.dimmed.dc, 0, 0, SRCCOPY);
  if (!r.IsEmptyArea() && a.doc.outline.empty())
    BitBlt(back, r.X, r.Y, r.Width, r.Height, d.original.dc, r.X, r.Y, SRCCOPY);
  {
    Graphics g(back);
    g.SetPageUnit(UnitPixel);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    auto mon = d.monitor(a.mouse);
    int unit = dpiMetric(1, a.mouse);
    int font = dpiMetric(13, a.mouse);
    Color accent(255, 158, 73, 213), panel(248, 43, 31, 53);
    if (!r.IsEmptyArea()) {
      drawSelected(g, a.doc, d, !a.doc.outline.empty());
      Pen border(accent, (REAL)std::max(1, unit));
      g.DrawRectangle(&border, r);
      if (a.doc.outline.size() > 2)
        g.DrawPolygon(&border, a.doc.outline.data(), (int)a.doc.outline.size());
      if (a.selected || a.drag == Selecting) {
        SolidBrush b(accent);
        int rad = dpiMetric(4, a.mouse);
        for (Point p : handles(r))
          g.FillEllipse(&b, p.X - rad, p.Y - rad, rad * 2, rad * 2);
      }
      auto dim = std::to_wstring(r.Width) + L" × " + std::to_wstring(r.Height) +
                 L" px";
      int width = (int)dim.size() * font * 2 / 3 + font, ht = font * 2;
      Rect badge(std::clamp(r.GetRight() - width, mon.X,
                            std::max(mon.X, mon.GetRight() - width)),
                 std::clamp(r.Y - ht - 5, mon.Y,
                            std::max(mon.Y, mon.GetBottom() - ht)),
                 width, ht);
      pill(g, badge, panel, 6);
      label(g, dim, badge, (float)font, Color::White);
    }
    if (pick) {
      auto c = d.sample(a.mouse);
      int z = dpiMetric(8, a.mouse), w = z * 19, ht = z * 18;
      Rect card(std::clamp(a.mouse.X + 18, mon.X,
                           std::max(mon.X, mon.GetRight() - w)),
                std::clamp(a.mouse.Y + 18, mon.Y,
                           std::max(mon.Y, mon.GetBottom() - ht)),
                w, ht);
      pill(g, card, panel, z);
      int x = card.X + (w - 9 * z) / 2, y = card.Y + z;
      for (int j = -4; j <= 4; ++j)
        for (int i = -4; i <= 4; ++i)
          solid(g, Rect(x + (i + 4) * z, y + (j + 4) * z, z, z),
                d.sample(offset(a.mouse, i, j)));
      Pen white(Color::White, 1);
      g.DrawRectangle(&white, x + 4 * z, y + 4 * z, z, z);
      label(g, colorString(c), Rect(card.X, card.Y + 10 * z, w, 3 * z),
            (float)font + 2, Color::White);
      auto rgb = L"RGB " + std::to_wstring(c.GetR()) + L", " +
                 std::to_wstring(c.GetG()) + L", " + std::to_wstring(c.GetB());
      label(g, rgb, Rect(card.X, card.Y + 13 * z, w, 4 * z), (float)font,
            Color::White);
    }
    layout();
    const Button *hover = nullptr;
    for (auto &b : a.buttons) {
      bool active = b.id == ModeBase + a.mode || b.id == ToolBase + a.tool;
      bool over = b.r.Contains(a.mouse);
      if (over)
        hover = &b;
      Color bg = active ? Color(255, 131, 60, 181)
                 : over ? Color(255, 100, 72, 115)
                        : panel;
      if (b.id == PaintColor)
        bg = a.ink;
      pill(g, b.r, bg, b.glyph ? b.r.Height / 2 : dpiMetric(7, a.mouse));
      bool disabled =
          (b.id == Undo && a.undo.empty()) || (b.id == Redo && a.redo.empty());
      if (!drawToolSymbol(g, b))
        label(g, b.caption, b.r, (float)(b.glyph ? b.r.Height / 2 : font),
              disabled ? Color(255, 150, 143, 154) : Color::White, true,
              b.glyph ? L"Segoe MDL2 Assets" : L"Segoe UI");
    }
    if (hover && hover->glyph) {
      int ww = (int)hover->tip.size() * font * 2 / 3 + font, hh = font * 2;
      Rect tip(
          std::clamp(hover->r.X, mon.X, std::max(mon.X, mon.GetRight() - ww)),
          hover->r.Y - hh - 4, ww, hh);
      if (tip.Y < mon.Y)
        tip.Y = hover->r.GetBottom() + 4;
      pill(g, tip, panel, 4);
      label(g, hover->tip, tip, (float)font, Color::White);
    }
  }
  BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
         ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
         back, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
  EndPaint(h, &ps);
}

static void complete(bool copy) {
  if (!s || !s->selected)
    return;
  auto &a = *s;
  auto r = a.doc.selection;
  auto pixels = renderPixels(a.doc, a.desktop);
  if (pixels.empty()) {
    MessageBoxW(overlay, L"Unable to render this selection.", L"Captura",
                MB_OK);
    return;
  }
  wstring folder = prefs.dir;
  while (a.saved.empty()) {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stem[80];
    swprintf_s(stem, L"Captura %04u-%02u-%02u %02u-%02u-%02u.%03u", time.wYear,
               time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
               time.wMilliseconds);
    vector<wchar_t> dest(32768);
    if (captura_save(folder.c_str(), stem, pixels.data(), r.Width, r.Height,
                     dest.data(), dest.size())) {
      a.saved = dest.data();
      break;
    }
    auto msg = wstring(L"Could not save the screenshot.\n\n") + dest.data() +
               L"\n\nChoose another folder?";
    if (MessageBoxW(overlay, msg.c_str(), L"Captura",
                    MB_YESNO | MB_ICONWARNING) != IDYES)
      return;
    folder = folderPicker(overlay, folder);
    if (folder.empty())
      return;
  }
  if (copy && !copyImage(overlay, pixels, r.Width, r.Height)) {
    MessageBoxW(overlay,
                L"Your PNG is saved. The clipboard is busy. Use Copy to retry.",
                L"Captura", MB_OK);
    return;
  }
  notify(copy ? L"Screenshot saved and copied" : L"Screenshot saved", a.saved);
  finish();
}
struct Recognition {
  wstring text;
  bool ok;
};
static void extract() {
  if (!s || !s->selected || recognizing)
    return;
  auto r = s->doc.selection;
  auto pixels = renderPixels(s->doc, s->desktop);
  if (pixels.empty())
    return;
  recognizing = true;
  SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  std::thread([data = std::move(pixels), r]() {
    bool ok = false;
    auto raw = captura_ocr(data.data(), r.Width, r.Height, &ok);
    auto result = new Recognition{raw, ok};
    captura_free(raw);
    if (!PostMessageW(host, OcrDone, 0, (LPARAM)result))
      delete result;
  }).detach();
}
static void history(bool redo) {
  auto &a = *s;
  auto &from = redo ? a.redo : a.undo;
  auto &to = redo ? a.undo : a.redo;
  if (from.empty())
    return;
  to.push_back(std::move(a.doc));
  a.doc = std::move(from.back());
  from.pop_back();
  a.selected = !a.doc.selection.IsEmptyArea();
  a.saved.clear();
  a.chosen = -1;
  refresh();
}
static void action(int id) {
  if (!s || recognizing)
    return;
  auto &a = *s;
  if (id >= ModeBase && id < ModeBase + 6) {
    changeMode((Mode)(id - ModeBase));
    return;
  }
  if (id >= ToolBase && id < ToolBase + 11) {
    a.tool = (Tool)(id - ToolBase);
    a.chosen = -1;
    refresh();
    return;
  }
  switch (id) {
  case Cancel:
    finish();
    return;
  case Settings:
    settings();
    return;
  case Save:
    complete(false);
    return;
  case Copy:
    complete(true);
    return;
  case Undo:
    history(false);
    return;
  case Redo:
    history(true);
    return;
  case Recognize:
    extract();
    return;
  case PaintColor: {
    static COLORREF colors[16]{};
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = overlay;
    cc.lpCustColors = colors;
    cc.rgbResult = RGB(a.ink.GetR(), a.ink.GetG(), a.ink.GetB());
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&cc)) {
      a.ink = Color(255, GetRValue(cc.rgbResult), GetGValue(cc.rgbResult),
                    GetBValue(cc.rgbResult));
      if (a.chosen >= 0) {
        a.before = a.doc;
        a.doc.marks[a.chosen].color = a.ink;
        a.checkpoint();
      }
    }
    break;
  }
  case Weight: {
    HMENU menu = CreatePopupMenu();
    int widths[] = {2, 4, 8, 12};
    for (int w : widths)
      AppendMenuW(menu, MF_STRING | (w == a.stroke ? MF_CHECKED : 0), w,
                  (std::to_wstring(w) + L" px").c_str());
    POINT p{};
    GetCursorPos(&p);
    int w = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0,
                           overlay, nullptr);
    DestroyMenu(menu);
    if (w) {
      a.stroke = w;
      if (a.chosen >= 0) {
        a.before = a.doc;
        a.doc.marks[a.chosen].width = w;
        a.checkpoint();
      }
    }
    break;
  }
  }
  refresh();
}
static int hitMark(Point p) {
  auto &a = *s;
  for (int i = (int)a.doc.marks.size() - 1; i >= 0; --i) {
    auto &m = a.doc.marks[i];
    auto first = m.points.front();
    auto last = m.points.back();
    if (m.tool == Text) {
      HDC dc = GetDC(overlay);
      Graphics g(dc);
      FontFamily ff(L"Segoe UI");
      Font f(&ff, (REAL)(14 + m.width * 2), FontStyleRegular, UnitPixel);
      RectF box;
      g.MeasureString(m.text.c_str(), -1, &f,
                      PointF((REAL)first.X, (REAL)first.Y), &box);
      ReleaseDC(overlay, dc);
      if (box.Contains((REAL)p.X, (REAL)p.Y))
        return i;
      continue;
    }
    if (m.tool == Number) {
      if (withinRadius(first, p, 12 + m.width))
        return i;
      continue;
    }
    Rect r = bounds(first, last);
    int pad = std::max(5, m.width);
    if (m.tool == Filled || m.tool == Mosaic) {
      if (r.Contains(p))
        return i;
      continue;
    }
    if (m.tool == Box || m.tool == Oval) {
      GraphicsPath path;
      if (m.tool == Box)
        path.AddRectangle(r);
      else
        path.AddEllipse(r);
      Pen pen(Color::Black, (REAL)(2 * pad));
      if (path.IsOutlineVisible(p, &pen))
        return i;
      continue;
    }
    if (m.points.size() == 1) {
      if (withinRadius(first, p, pad))
        return i;
      continue;
    }
    GraphicsPath path;
    path.AddLines(m.points.data(), (int)m.points.size());
    Pen pen(Color::Black,
            (REAL)(m.tool == Highlighter ? 4 * m.width + 4 : 2 * pad));
    if (path.IsOutlineVisible(p, &pen))
      return i;
  }
  return -1;
}
static void pointerDown(Point p) {
  if (recognizing)
    return;
  auto &a = *s;
  a.mouse = p;
  layout();
  for (const auto &b : a.buttons)
    if (b.r.Contains(p)) {
      int id = b.id;
      action(id);
      return;
    }
  if (a.mode == PickerMode) {
    auto hex = colorString(a.desktop.sample(p));
    if (copyText(overlay, hex)) {
      notify(L"Color copied", hex);
      finish();
    } else
      MessageBoxW(overlay, L"The clipboard is busy. Click to retry.",
                  L"Captura", MB_OK);
    return;
  }
  a.before = a.doc;
  a.start = p;
  if (!a.selected && (a.mode == WindowMode || a.mode == MonitorMode)) {
    if (!a.doc.selection.IsEmptyArea()) {
      a.before = {};
      a.selected = true;
      a.checkpoint();
    }
    refresh();
    return;
  }
  if (a.selected) {
    auto hs = handles(a.doc.selection);
    for (int i = 0; i < 8; ++i)
      if (withinRadius(hs[i], p, dpiMetric(6, p))) {
        a.drag = Resizing;
        a.handle = i;
        SetCapture(overlay);
        refresh();
        return;
      }
    if (a.doc.selection.Contains(p)) {
      if (a.tool == Move) {
        a.chosen = hitMark(p);
        a.drag = a.chosen >= 0 ? MovingMark : Moving;
        SetCapture(overlay);
        return;
      }
      Mark m;
      m.tool = a.tool;
      m.color = a.ink;
      m.width = a.stroke;
      m.points.push_back(p);
      if (a.tool == Text) {
        TextDialog dlg;
        if (!textDialog(overlay, dlg) || dlg.value.empty())
          return;
        m.text = dlg.value;
      }
      if (a.tool == Number) {
        int n = 1;
        for (auto &mark : a.doc.marks)
          if (mark.tool == Number)
            ++n;
        m.text = std::to_wstring(n);
      }
      a.doc.marks.push_back(std::move(m));
      a.chosen = -1;
      if (a.tool == Text || a.tool == Number) {
        a.checkpoint();
        refresh();
        return;
      }
      a.drag = Drawing;
      SetCapture(overlay);
      refresh();
      return;
    }
  }
  a.chosen = -1;
  a.selected = false;
  a.doc = {};
  a.drag = Selecting;
  if (a.mode == FreeMode)
    a.doc.outline.push_back(p);
  SetCapture(overlay);
  refresh();
}
static void pointerMove(Point p) {
  if (recognizing)
    return;
  auto &a = *s;
  if (a.drag != Idle) {
    p.X = std::clamp(p.X, 0, a.desktop.area.Width);
    p.Y = std::clamp(p.Y, 0, a.desktop.area.Height);
  }
  a.mouse = p;
  int dx = p.X - a.start.X, dy = p.Y - a.start.Y;
  switch (a.drag) {
  case Idle:
    if (!a.selected && (a.mode == MonitorMode || a.mode == WindowMode)) {
      a.doc.selection = {};
      if (a.mode == MonitorMode)
        a.doc.selection = a.desktop.monitor(p);
      else
        for (auto r : a.desktop.windows)
          if (r.Contains(p)) {
            a.doc.selection = r;
            break;
          }
    }
    break;
  case Selecting:
    if (a.mode == FreeMode) {
      auto &pts = a.doc.outline;
      if (pts.empty() || !withinRadius(pts.back(), p, 0))
        pts.push_back(p);
      if (pts.size() == 1)
        a.doc.selection = Rect(p.X, p.Y, 1, 1);
      else {
        Rect r = a.doc.selection;
        if (r.IsEmptyArea())
          r = Rect(a.start.X, a.start.Y, 1, 1);
        Rect::Union(a.doc.selection, r,
                    Rect(std::min(p.X, a.desktop.area.Width - 1),
                         std::min(p.Y, a.desktop.area.Height - 1), 1, 1));
      }
    } else
      a.doc.selection = bounds(a.start, p);
    break;
  case Moving: {
    auto r = a.before.selection;
    r.X = std::clamp(r.X + dx, 0, a.desktop.area.Width - r.Width);
    r.Y = std::clamp(r.Y + dy, 0, a.desktop.area.Height - r.Height);
    changeBounds(a.doc, a.before, r, true);
    break;
  }
  case MovingMark: {
    auto &mark = a.doc.marks[a.chosen];
    mark.points = a.before.marks[a.chosen].points;
    for (auto &q : mark.points)
      q = offset(q, dx, dy);
    break;
  }
  case Resizing: {
    auto r = a.before.selection;
    int l = r.X, t = r.Y, right = r.GetRight(), b = r.GetBottom(), i = a.handle;
    if (i == 0 || i == 6 || i == 7)
      l = std::min(right - 1, p.X);
    if (i == 2 || i == 3 || i == 4)
      right = std::max(l + 1, p.X);
    if (i <= 2)
      t = std::min(b - 1, p.Y);
    if (i >= 4 && i <= 6)
      b = std::max(t + 1, p.Y);
    changeBounds(a.doc, a.before, Rect(l, t, right - l, b - t), false);
    break;
  }
  case Drawing: {
    auto &m = a.doc.marks.back();
    if (m.tool == PenTool || m.tool == Highlighter) {
      if (!withinRadius(m.points.back(), p, 0))
        m.points.push_back(p);
    } else if (m.points.size() == 1)
      m.points.push_back(p);
    else
      m.points.back() = p;
    break;
  }
  }
  refresh();
}
static void pointerUp(Point p) {
  auto &a = *s;
  if (a.drag == Idle)
    return;
  auto drag = a.drag;
  pointerMove(p);
  a.drag = Idle;
  ReleaseCapture();
  if (drag == Selecting && (a.doc.selection.IsEmptyArea() ||
                            (a.mode == FreeMode && a.doc.outline.size() < 3))) {
    a.doc = a.before;
    a.selected = !a.doc.selection.IsEmptyArea();
    refresh();
    return;
  }
  a.selected = true;
  if (drag == Drawing || drag == Selecting || !withinRadius(a.start, p, 0))
    a.checkpoint();
  refresh();
  if (a.mode == OcrMode && drag == Selecting)
    extract();
}
static LRESULT CALLBACK overlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (!s)
    return DefWindowProcW(h, msg, wp, lp);
  Point p(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
  switch (msg) {
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT:
    paint(h);
    return 0;
  case WM_DISPLAYCHANGE:
    if (!recognizing && IsWindowEnabled(h))
      finish();
    return 0;
  case WM_MOUSEMOVE:
    pointerMove(p);
    return 0;
  case WM_LBUTTONDOWN:
    pointerDown(p);
    return 0;
  case WM_LBUTTONUP:
    pointerUp(p);
    return 0;
  case WM_CAPTURECHANGED:
    if (s->drag != Idle) {
      s->doc = s->before;
      s->selected = !s->doc.selection.IsEmptyArea();
      s->drag = Idle;
      refresh();
    }
    return 0;
  case WM_LBUTTONDBLCLK: {
    if (recognizing || s->tool != Move)
      return 0;
    int i = hitMark(p);
    if (i >= 0 && s->doc.marks[i].tool == Text) {
      TextDialog dlg{s->doc.marks[i].text};
      if (textDialog(h, dlg) && !dlg.value.empty()) {
        s->before = s->doc;
        s->doc.marks[i].text = dlg.value;
        s->checkpoint();
        refresh();
      }
    }
    return 0;
  }
  case WM_KEYDOWN: {
    if (recognizing)
      return 0;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (wp == VK_ESCAPE) {
      finish();
      return 0;
    }
    if (s->drag != Idle)
      return 0;
    if (wp == VK_RETURN || ctrl && wp == 'S')
      action(Save);
    else if (ctrl && wp == 'C')
      action(Copy);
    else if (ctrl && wp == 'Z')
      action(GetKeyState(VK_SHIFT) < 0 ? Redo : Undo);
    else if (ctrl && wp == 'Y')
      action(Redo);
    else if (wp == VK_DELETE && s->chosen >= 0) {
      s->before = s->doc;
      s->doc.marks.erase(s->doc.marks.begin() + s->chosen);
      s->chosen = -1;
      s->checkpoint();
      refresh();
    } else if (!ctrl && wp >= '1' && wp <= '6')
      changeMode((Mode)(wp - '1'));
    return 0;
  }
  case WM_CLOSE:
    if (!recognizing)
      finish();
    return 0;
  case WM_SETCURSOR: {
    auto cursor = IDC_CROSS;
    if (recognizing)
      cursor = IDC_WAIT;
    else if (std::any_of(
                 s->buttons.begin(), s->buttons.end(),
                 [](const Button &b) { return b.r.Contains(s->mouse); }))
      cursor = IDC_HAND;
    else if (s->selected && s->tool == Move &&
             s->doc.selection.Contains(s->mouse))
      cursor = IDC_SIZEALL;
    SetCursor(LoadCursorW(nullptr, cursor));
    return TRUE;
  }
  }
  return DefWindowProcW(h, msg, wp, lp);
}
static void begin() {
  if (modal || recognizing || (overlay && !IsWindowEnabled(overlay)))
    return;
  if (overlay) {
    SetForegroundWindow(overlay);
    return;
  }
  auto next = make_unique<Session>();
  if (!next->desktop.freeze()) {
    notify(L"Capture unavailable",
           L"Windows could not capture the current desktop.");
    return;
  }
  POINT p{};
  GetCursorPos(&p);
  auto r = next->desktop.area;
  next->mouse = next->anchor = Point(p.x - r.X, p.y - r.Y);
  s = std::move(next);
  overlay = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, overlayClass,
                            L"Captura capture", WS_POPUP, r.X, r.Y, r.Width,
                            r.Height, nullptr, nullptr, instance, nullptr);
  if (!overlay) {
    s.reset();
    return;
  }
  ShowWindow(overlay, SW_SHOW);
  SetForegroundWindow(overlay);
  SetFocus(overlay);
  UpdateWindow(overlay);
}
static void addTray() {
  tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  Shell_NotifyIconW(NIM_ADD, &tray);
}
static LRESULT CALLBACK hostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (taskbarCreated && msg == taskbarCreated) {
    addTray();
    return 0;
  }
  switch (msg) {
  case Activate:
  case WM_HOTKEY:
    begin();
    return 0;
  case OcrDone: {
    unique_ptr<Recognition> result((Recognition *)lp);
    recognizing = false;
    if (!s)
      return 0;
    if (!result->ok || result->text.empty())
      MessageBoxW(overlay,
                  result->ok ? L"No text was found in this selection."
                             : result->text.c_str(),
                  L"Captura", MB_OK);
    else {
      TextDialog dlg{result->text, true};
      textDialog(overlay, dlg);
    }
    refresh();
    return 0;
  }
  case TrayMessage:
    if (lp == WM_LBUTTONUP)
      begin();
    if (lp == WM_RBUTTONUP && !modal && !recognizing &&
        (!overlay || IsWindowEnabled(overlay))) {
      HMENU menu = CreatePopupMenu();
      AppendMenuW(menu, MF_STRING, 1, L"New capture");
      AppendMenuW(menu, MF_STRING, 2, L"Settings…");
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, 3, L"Exit Captura");
      POINT p{};
      GetCursorPos(&p);
      SetForegroundWindow(h);
      int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y,
                              0, h, nullptr);
      DestroyMenu(menu);
      PostMessageW(h, WM_NULL, 0, 0);
      if (id == 1)
        begin();
      if (id == 2)
        settings();
      if (id == 3)
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
    return 0;
  case WM_CLOSE:
    if (!recognizing && !modal && (!overlay || IsWindowEnabled(overlay))) {
      finish();
      DestroyWindow(h);
    }
    return 0;
  case WM_DESTROY:
    Shell_NotifyIconW(NIM_DELETE, &tray);
    UnregisterHotKey(h, 1);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}
extern "C" int captura_run() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  HANDLE mutex =
      CreateMutexW(nullptr, FALSE, L"Local\\projectGloria.Captura.Singleton");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND prior = nullptr;
    for (int i = 0; i < 50 && !prior; ++i) {
      prior = FindWindowW(hostClass, nullptr);
      if (!prior)
        Sleep(20);
    }
    if (prior) {
      DWORD pid = 0;
      GetWindowThreadProcessId(prior, &pid);
      AllowSetForegroundWindow(pid);
      PostMessageW(prior, Activate, 0, 0);
    }
    CloseHandle(mutex);
    return 0;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX ctrls{sizeof(ctrls), ICC_HOTKEY_CLASS};
  InitCommonControlsEx(&ctrls);
  GdiplusStartupInput input;
  ULONG_PTR token = 0;
  if (GdiplusStartup(&token, &input, nullptr) != Ok)
    return 1;
  instance = GetModuleHandleW(nullptr);
  prefs.load();
  HICON icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0,
                                 0, LR_DEFAULTSIZE | LR_SHARED);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.hInstance = instance;
  wc.hIcon = icon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = hostClass;
  wc.lpfnWndProc = hostProc;
  RegisterClassExW(&wc);
  wc.lpszClassName = overlayClass;
  wc.lpfnWndProc = overlayProc;
  wc.style = CS_DBLCLKS;
  RegisterClassExW(&wc);
  host = CreateWindowExW(WS_EX_TOOLWINDOW, hostClass, L"Captura", WS_POPUP, 0,
                         0, 0, 0, nullptr, nullptr, instance, nullptr);
  if (!host)
    return 1;
  tray.cbSize = sizeof(tray);
  tray.hWnd = host;
  tray.uID = 1;
  tray.uCallbackMessage = TrayMessage;
  tray.hIcon = icon;
  wcscpy_s(tray.szTip, L"Captura");
  addTray();
  taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
  registered = registerKey(prefs.key);
  if (!registered)
    notify(L"Capture shortcut unavailable",
           L"Choose a different shortcut in Captura settings.");
  PostMessageW(host, Activate, 0, 0);
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  GdiplusShutdown(token);
  CoUninitialize();
  if (mutex)
    CloseHandle(mutex);
  return 0;
}
