//! case: s32_5_0021
//! doc: spec
//! section: 32.5
//! title: env: IntPtr as ordinary callback parameter name must compile
//! rule: A parameter named `env: IntPtr` with a type annotation is not the bare
//!   env-marker; the following `:` disambiguates. `env` is a regular parameter
//!   name in a function type inside an extern declaration.
//! cites: 32.5, 32.8
//! expect: accept
module Conf;
import System.Interop;

extern "libexample"
  method Walk(handle: IntPtr,
      pred: (env: IntPtr, code: int) -> bool): int;
end;

public static class Use
  public static method DoWalk(h: IntPtr): int
    return Walk(h, lambda(env: IntPtr, code: int) => do
      _ := env;
      return code > 0;
    end);
  end DoWalk;
end Use;
