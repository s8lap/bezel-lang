//! case: s7_4_0117
//! doc: spec
//! section: 7.4
//! title: the false branch of an or composition sees the left operand's false-facts
//! rule: spec §7.4: "boolean composition of tests: in `A and B`, `B` (and the
//!     true branch) is checked under `A`'s true-facts; in `A or B`, `B` (and the
//!     false branch) sees `A`'s false-facts". On the false path of `r = null or
//!     false` the left operand's null test was false, so `r` is non-null there.
//!     The local itself is never narrowed — §7.4 narrows a declaration only
//!     from a non-nullable initializer, and this one is nullable-typed — so the
//!     composition is what narrows it.
//! cites: 7.4
//! expect: run
//! stdout: ok
module Conf;

public class Program
  public static method Take(radix: int): string
    return "ok";
  end Take;

  public static method Radix(): int?
    return 16;
  end Radix;

  public static method Main()
    var r: int? := Radix();
    if r = null or false then
      Console.WriteLine("null");
    else
      Console.WriteLine(Take(r));
    end;
  end Main;
end Program;
