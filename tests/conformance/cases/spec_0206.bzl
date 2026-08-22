class FileStream
  private _handle: IntPtr;

  public constructor(path: string)
    using var cpath := CString.From(path);
    _handle := NativeOpen(cpath);
  end constructor;

  destructor
    if _handle <> IntPtr.Zero then
      NativeClose(_handle);
      _handle := IntPtr.Zero;
    end;
  end destructor;
end FileStream;
