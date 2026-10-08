//! case: s28_11_0003
//! doc: spec
//! section: 28.11
//! title: move on a borrowing collection slot is rejected — there is no ownership to transfer
//! rule: "No other receiver admits it ... A copy, shared-reference or
//!   borrow slot, and a read-only view, admit none: ... the borrowing
//!   collections ... reject `move coll[i]` at compile time."
//! cites: 28.11
//! expect: reject
//! diagnostic: move
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var arr: Array<Widget> := [new Widget()];
    var w := move arr[0];
  end Main;
end Program;
