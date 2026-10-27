//! case: s22_6_0110
//! doc: spec
//! section: 22.6
//! title: a compile-time integer constant is read at its own static type
//! rule: spec §22.6: "A **compile-time integer constant** (§17.14) is read at
//!     its own static type too — `int` for an unsuffixed literal that fits
//!     `int` (§4.2), a `const`'s declared type". The `const` is declared `long`,
//!     so it is exact at the `long` parameter and converts at the `int` one:
//!     the `long` candidate is the only exact match and is selected.
//! cites: 22.6, 17.14
//! expect: run
//! stdout: long
module Conf;

public class Program
  public static method M(x: int): string
    return "int";
  end M;

  public static method M(x: long): string
    return "long";
  end M;

  public static method Main()
    const N: long := 5;
    Console.WriteLine(M(N));
  end Main;
end Program;
