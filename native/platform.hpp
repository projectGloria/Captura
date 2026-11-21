#pragma once
#include "editor.hpp"
extern "C" bool captura_save(const wchar_t *, const wchar_t *, const BYTE *,
                             unsigned, unsigned, wchar_t *, size_t);
extern "C" wchar_t *captura_ocr(const BYTE *, int, int, bool *);
extern "C" void captura_free(wchar_t *);
inline wstring known(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)))
    return {};
  wstring result(p);
  CoTaskMemFree(p);
  return result;
}
inline wstring field(HWND window, int id) {
  HWND e = GetDlgItem(window, id);
  wstring s(GetWindowTextLengthW(e) + 1, 0);
  s.resize(GetWindowTextW(e, s.data(), (int)s.size()));
  return s;
}
inline wstring folderPicker(HWND owner, const wstring &current) {
  IFileOpenDialog *dlg = nullptr;
  wstring result;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                 CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    DWORD flags = 0;
    dlg->GetOptions(&flags);
    dlg->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose a screenshot folder");
    IShellItem *item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr,
                                              IID_PPV_ARGS(&item)))) {
      dlg->SetFolder(item);
      item->Release();
    }
    if (SUCCEEDED(dlg->Show(owner)) && SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        result = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
    dlg->Release();
  }
  return result;
}
inline bool clipboard(HWND owner, UINT format, const void *bytes,
                      size_t length) {
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, length);
  if (!mem)
    return false;
  auto ptr = GlobalLock(mem);
  if (!ptr) {
    GlobalFree(mem);
    return false;
  }
  memcpy(ptr, bytes, length);
  GlobalUnlock(mem);
  bool opened = false;
  for (int i = 0; i < 5 && !opened; ++i) {
    opened = !!OpenClipboard(owner);
    if (!opened)
      Sleep(12);
  }
  bool ok = opened && EmptyClipboard() && SetClipboardData(format, mem);
  if (opened)
    CloseClipboard();
  if (!ok)
    GlobalFree(mem);
  return ok;
}
inline bool copyText(HWND owner, const wstring &s) {
  return clipboard(owner, CF_UNICODETEXT, s.c_str(),
                   (s.size() + 1) * sizeof(wchar_t));
}
inline bool copyImage(HWND owner, const vector<BYTE> &pixels, int w, int h) {
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = w;
  header.bV5Height = -h;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5CSType = LCS_sRGB;
  header.bV5RedMask = 0xff0000;
  header.bV5GreenMask = 0xff00;
  header.bV5BlueMask = 0xff;
  header.bV5AlphaMask = 0xff000000;
  vector<BYTE> dib(sizeof(header) + pixels.size());
  memcpy(dib.data(), &header, sizeof(header));
  auto dst = dib.data() + sizeof(header);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    dst[i] = pixels[i + 2];
    dst[i + 1] = pixels[i + 1];
    dst[i + 2] = pixels[i];
    dst[i + 3] = pixels[i + 3];
  }
  return clipboard(owner, CF_DIBV5, dib.data(), dib.size());
}
struct Preferences {
  wstring dir = known(FOLDERID_Pictures);
  WORD key = MAKEWORD('S', HOTKEYF_CONTROL | HOTKEYF_SHIFT);
  fs::path path = fs::path(known(FOLDERID_RoamingAppData)) /
                  L"com.gloria.captura" / L"settings.ini";
  void load() {
    vector<wchar_t> buf(32768);
    if (GetPrivateProfileStringW(L"Capture", L"Folder", L"", buf.data(),
                                 (DWORD)buf.size(), path.c_str()))
      dir = buf.data();
    key =
        (WORD)GetPrivateProfileIntW(L"Capture", L"Shortcut", key, path.c_str());
  }
  bool save() {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
      return false;
    auto temp = path;
    temp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    const wstring contents = L"\ufeff[Capture]\r\nFolder=" + dir +
                             L"\r\nShortcut=" + std::to_wstring(key) + L"\r\n";
    file.write(reinterpret_cast<const char *>(contents.data()),
               contents.size() * sizeof(wchar_t));
    file.close();
    if (file && MoveFileExW(temp.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      return true;
    fs::remove(temp, ec);
    return false;
  }
};
struct TextDialog {
  wstring value;
  bool result = false;
};
static INT_PTR CALLBACK textProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  auto *d = (TextDialog *)GetWindowLongPtrW(h, DWLP_USER);
  if (msg == WM_INITDIALOG) {
    d = (TextDialog *)lp;
    SetWindowLongPtrW(h, DWLP_USER, lp);
    SetWindowTextW(h, d->result ? L"Extracted text" : L"Annotation text");
    SetDlgItemTextW(h, 101, d->value.c_str());
    SetDlgItemTextW(h, 1, d->result ? L"Copy text" : L"Apply");
    if (d->result) {
      SendDlgItemMessageW(h, 101, EM_SETREADONLY, TRUE, 0);
      SetDlgItemTextW(h, 2, L"Close");
    }
    SetFocus(GetDlgItem(h, 101));
    SendDlgItemMessageW(h, 101, EM_SETSEL, 0, -1);
    return FALSE;
  }
  if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
    d->value = field(h, 101);
    if (d->result && !copyText(h, d->value)) {
      MessageBoxW(h, L"The clipboard is busy. Try copying again.", L"Captura",
                  MB_OK);
      return TRUE;
    }
    EndDialog(h, IDOK);
    return TRUE;
  }
  if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
    EndDialog(h, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}
inline bool textDialog(HWND owner, TextDialog &d) {
  return DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(202),
                         owner, textProc, (LPARAM)&d) == IDOK;
}
