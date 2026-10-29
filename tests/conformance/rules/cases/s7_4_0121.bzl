//! case: s7_4_0121
//! doc: spec
//! section: 7.4
//! title: a nullable local narrowed by an early return passes as its non-null int to a stdlib call
//! rule: "Narrowing also applies after early return" and "an early exit: after
//!   `if x is null then return; end;` … the surviving path is narrowed". On the
//!   surviving path `n: int?` is non-null, so `a.IndexOf(n)` takes it as an `int`
//!   argument, and `IndexOf` returns the zero-based index of the first match.
//!   (§7.4, v0.350; `IndexOf` is stdlib §4.4.)
//! cites: 7.4
//! expect: run
//! output: 0
//!   1
module Conf;

import System;

public class Program
  public static method Main()
    using var a: Array<int> := new Array<int>();
    a.Add(1);
    a.Add(2);
    var first: int? := 1;
    if first is null then return; end;
    Console.WriteLine(a.IndexOf(first).ToString());
    var second: int? := 2;
    if second is null then return; end;
    Console.WriteLine(a.IndexOf(second).ToString());
  end Main;
end Program;
