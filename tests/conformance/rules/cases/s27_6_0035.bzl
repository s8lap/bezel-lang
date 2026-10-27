//! case: s27_6_0035
//! doc: spec
//! section: 27.6
//! title: a runtime const local takes an ordinary method reference
//! rule: "Method references work for static methods, instance methods (the instance
//!   is captured at formation — by reference for class- or interface-typed
//!   receivers, by copy for value-shaped receivers)" (§27.6, v0.340), and §21.1
//!   makes a `const` local that fails the compile-time-const conditions a
//!   **runtime const** rather than an error — a binding, not an error, so the
//!   method reference over it forms and calls through.
//! cites: 27.6, 21.1
//! expect: run
//! stdout: 7
module Conf;

import System;

public class D
  public constructor()
  end constructor;

  public method Hi(): int
    return 7;
  end Hi;
end D;

public class Program
  public static method Main()
    var d := new D();
    const f := d.Hi;
    Console.WriteLine(f());
  end Main;
end Program;
