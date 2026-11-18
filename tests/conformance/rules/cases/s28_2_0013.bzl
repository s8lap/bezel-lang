//! case: s28_2_0013
//! doc: spec
//! section: 28.2
//! title: invoking a closure over a variable after its dominating disposal is a compile error
//! rule: "A closure that captures the variable itself, rather than a holder of its
//!   value, reads the variable when it is invoked (§27.5): its invocation after the
//!   kill is a use of the variable under the first sentence" (§28.2, v0.357) — and
//!   the first sentence makes a use the explicit disposal dominates a compile
//!   error.
//! cites: 28.2, 27.5
//! expect: reject
//! diagnostic: use-after-dispose
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
