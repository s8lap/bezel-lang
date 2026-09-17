//! case: s32_2_0001
//! doc: spec
//! section: 32.2
//! title: an extern method is called unqualified from within its declaring module
//! rule: From within the same module, they are called by name without qualification.
//! cites: 32.2
//! expect: accept
module Conf;

extern "libc"
  method getpid(): int;
end;

public static class ProcessInfo
  public static method Current(): int
    return getpid();
  end Current;
end ProcessInfo;
