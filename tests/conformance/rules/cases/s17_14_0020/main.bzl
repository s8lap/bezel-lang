//! case: s17_14_0020
//! doc: spec
//! section: 17.14
//! title: an imported const member is a constant expression, so it narrows by value
//! rule: spec §17.14: "A **constant expression** is built from exactly these
//!     forms: ... a reference to a `const` member — class-level, local, or the
//!     compiler-provided constants of the primitive wrapper types". The imported
//!     class-level `const` is a constant expression, so §5.1's constant
//!     narrowing reaches the `int` radix parameter by value — "across
//!     signedness" — and the call parses under radix 16.
//! cites: 17.14, 5.1, 9.1
//! expect: run
//! stdout: 255
module App;

import Lib;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("ff", Cfg.K));
  end Main;
end Program;
