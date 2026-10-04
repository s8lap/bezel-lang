//! case: s32_5_0020
//! doc: spec
//! section: 32.5
//! title: env as ordinary local variable name must compile
//! rule: `env` is not a keyword outside the bare-env-marker position; it may be used
//!   as an ordinary local variable name.
//! cites: 32.5, 32.8
//! expect: accept
module Conf;

public static class Control
  public static method Test(): int
    var env := 42;
    return env;
  end Test;
end Control;
