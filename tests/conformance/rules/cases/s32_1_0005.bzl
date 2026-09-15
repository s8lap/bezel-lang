//! case: s32_1_0005
//! doc: spec
//! section: 32.1
//! title: an extern block nested inside a class is rejected
//! rule: They are always module-level -- never members of classes.
//! cites: 32.1
//! expect: reject
//! diagnostic: module-level
module Conf;

public class Native
  extern "libc"
    public method getpid(): int;
  end;
end Native;
