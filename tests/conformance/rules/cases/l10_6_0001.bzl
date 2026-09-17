//! case: l10_6_0001
//! doc: stdlib
//! section: 10.6
//! title: Interop.CodeAddress accepts a capture-free static method reference
//! rule: F gated per-instantiation to FFI-compatible function types ...
//!   a static method reference or a non-capturing lambda.
//! cites: 10.6
//! expect: accept
module Conf;

import System.Interop;

public class Program
  public static method Callback(x: int): int => x;

  public method Demo()
    var addr := Interop.CodeAddress(Callback);
  end Demo;
end Program;
