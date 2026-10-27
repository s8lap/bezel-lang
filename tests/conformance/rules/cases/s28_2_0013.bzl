//! case: s28_2_0013
//! doc: spec
//! section: 28.2
//! title: a closure that captures a disposed local warns at the call that reads it
//! rule: "The compiler additionally must emit a compile-time warning when a local
//!   variable is used (read, borrowed from, or consumed) after an explicit
//!   `Dispose()`" (§28.2, v0.340). The closure captures a borrow of `d`, and the
//!   capture is read at invoke time — the call after the explicit `Dispose()`
//!   reads a disposed value, so the warning is required.
//! cites: 28.2, 28.9
//! expect: warn
//! diagnostic: disposed
module Conf;

import System;

public class D
  public constructor()
  end constructor;

  public method Hi(): int
    return 7;
  end Hi;
end D;

public class Demo
  public method Run()
    var d := new D();
    var g: () -> int := lambda() => d.Hi();
    d.Dispose();
    Console.WriteLine(g());
  end Run;
end Demo;
