//! case: s28_3_0209
//! doc: spec
//! section: 28.3
//! title: an interface-typed alias binding of an owned collection reads through the owner
//! rule: "An unmarked local binding or assignment RHS over a class- or
//!   interface-typed binding — `var y := x;`, `y := x;` — is a borrow of `x`,
//!   source `x` itself" (§28.3, v0.340). `il` is therefore a borrow of the owned
//!   `a`, not a second owner: it reads the owner's elements (three of them) and it
//!   takes no disposal obligation, so the owner's scope-end disposal disposes the
//!   collection exactly once and the program ends normally.
//! cites: 28.3, 28.6
//! expect: run
//! stdout: 3
module Conf;

import System;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    Console.WriteLine(il.Count);
  end Main;
end Program;
