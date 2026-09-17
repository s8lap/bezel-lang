//! case: s20_0013
//! doc: spec
//! section: 20
//! title: mixing an owned (moved) arm with a borrowed arm is rejected
//! rule: "**Ownership.** When the branches are class-typed, the expression's
//!   ownership is the arm-meet of §28.3's producers table: owned arms meet to
//!   owned, borrowed arms to borrowed with unioned source constraints, mixed arms
//!   are rejected; an unmarked read of a named owned binding written as a branch
//!   is a borrow arm, and the marked branch `move x` ... transfers."
//! cites: 20, 28.3
//! expect: reject
//! diagnostic: mixed
module Conf;

public class Resource
  public constructor()
  end constructor;
end Resource;

public class Program
  public static method Main()
    using var a := new Resource();
    using var b := new Resource();
    var r := (if true then move a else b);
  end Main;
end Program;
