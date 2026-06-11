extern "user32" convention "stdcall"
  public method MessageBoxW(hwnd: IntPtr, text: WString, caption: WString, style: uint): int;
end;
