//! case: s32_10_0005
//! doc: spec
//! section: 32.10
//! title: a static method reference may be handed to a native registration that retains it
//! rule: a static-method/non-capturing-lambda callback has no environment and its
//!   code address is process-lifetime — such a pointer *may* be retained by native
//!   code (signal handlers, event loops, plugin hooks) and remains valid until
//!   process exit; there is nothing to pin and nothing to unregister on the Bezel
//!   side.
//! cites: 32.10, 32.8
//! expect: accept
module Conf;

extern "libc"
  public method signal(sig: int, handler: (int) -> ()): IntPtr;
end;

public static class Handlers
  public static method OnSignal(sig: int)
  end OnSignal;

  public static method Install(sig: int): IntPtr
    return signal(sig, Handlers.OnSignal);
  end Install;
end Handlers;
