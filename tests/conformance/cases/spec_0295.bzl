import System.Interop;

public static class FileOps
  public static method OpenChecked(path: CString): int
    var fd := open(path, O_RDONLY);
    if fd < 0 then
      var e := Interop.LastErrno();          // read FIRST — the interpolation below allocates,
                                             //   and allocation is a runtime operation that may
                                             //   clobber the thread-local (stdlib §10.6)
      throw new IOException($"open failed: errno={e}");
    end;
    return fd;
  end OpenChecked;
end FileOps;
