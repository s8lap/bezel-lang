//! case: s32_9_0001
//! doc: spec
//! section: 32.9
//! title: Interop.LastErrno is read synchronously before the error message is built
//! rule: `Interop.LastErrno()` reads the calling thread's C `errno` directly. It is
//!   valid only when read before the next extern call or runtime operation on the
//!   thread.
//! cites: 32.9
//! expect: accept
module Conf;
import System.Interop;

extern "libc"
  method open(path: CString, flags: int): int;
end;

public static class FileOps
  public static method OpenChecked(path: CString): int
    var fd := open(path, 0);
    if fd < 0 then
      var e := Interop.LastErrno();
      throw new InvalidOperationException($"open failed: errno={e}");
    end;
    return fd;
  end OpenChecked;
end FileOps;
