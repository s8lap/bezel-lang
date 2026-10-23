//! case: s27_7_1_0009
//! doc: spec
//! section: 27.7.1
//! title: two method references over a value-shaped receiver are unequal
//! rule: "A method reference equals another method reference iff both denote the
//!   same method and, for instance references, are bound to the same receiver"
//!   (§27.7.1, v0.340), and a value-shaped receiver — a record, an enum, a
//!   copyable choice, a non-owning inline array, a primitive, the unit value, a
//!   `string`, a function value — has no receiver identity: each evaluation
//!   captures its own copy, so the two references are unequal even when the head
//!   is the same binding.
//! cites: 27.7.1, 27.6
//! expect: run
//! stdout: False
module Conf;

import System;

public record RThing
  public method Bump(): int
    return 7;
  end Bump;
end RThing;

public class Program
  public static method Main()
    var t: RThing := new RThing();
    var f: () -> int := t.Bump;
    var g: () -> int := t.Bump;
    Console.WriteLine(f = g);
  end Main;
end Program;
