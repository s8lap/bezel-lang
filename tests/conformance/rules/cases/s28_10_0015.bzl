//! case: s28_10_0015
//! doc: spec
//! section: 28.10
//! title: an unmarked using var over a named owned binding is a compile error
//! rule: "An unmarked `using var y := x;` is a compile error when its right-hand
//!   side is a named owned binding whose declared type, nullability stripped, is a
//!   class or interface type" (§28.10, v0.340) — the covered spelling does not
//!   say whether `y` adopts `x` or views it, and the compiler does not choose.
//!   `a` is a local whose ownership state is owned (the collection literal is an
//!   owned production, §28.3's producers table) and whose declared type
//!   `Array<int>` is a class type, so `using var b := a;` is rejected. KNOWN
//!   DIVERGENCE (#5582): the compiler admits the program and draws a spurious
//!   "`using var` `b` is vacuous" warning.
//! cites: 28.10, 28.3
//! expect: reject
//! diagnostic: using var
//! issue: 5582
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    using var b: Array<int> := a;
  end Main;
end Program;
