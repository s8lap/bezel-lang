//! case: s32_1_0004
//! doc: spec
//! section: 32.1
//! title: an extern method declared static is rejected
//! rule: They may not carry `static`, `virtual`, `override`, `abstract`, or `sealed`.
//! cites: 32.1
//! expect: reject
//! diagnostic: static
module Conf;

extern "libc"
  public static method getpid(): int;
end;
