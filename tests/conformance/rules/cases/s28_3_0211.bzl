//! case: s28_3_0211
//! doc: spec
//! section: 28.3
//! title: an IEnumerable-typed alias binding of an owned collection reads through the owner
//! rule: "An unmarked local binding or assignment RHS over a class- or
//!   interface-typed binding — `var y := x;`, `y := x;` — is a borrow of `x`,
//!   source `x` itself" (§28.3, v0.340). `e` is therefore a borrow of the owned
//!   `a`, not a second owner: iterating it yields the owner's three elements and
//!   it takes no disposal obligation, so the owner's scope-end disposal disposes
//!   the collection exactly once and the program ends normally.
//! cites: 28.3, 28.6
//! expect: run
//! stdout: 3
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var e: IEnumerable<int> := a;
    var n := 0;
    for var x in e do
      n := n + 1;
    end;
    Console.WriteLine(n);
  end Main;
end Program;
