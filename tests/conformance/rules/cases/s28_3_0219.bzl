//! case: s28_3_0219
//! doc: spec
//! section: 28.3
//! title: a self-assignment through the null-forgiving peel is rejected
//! rule: Over a named owned binding, `x!` is classified by its position exactly
//!   as the bare read `x` is, and at a local binding or assignment an unmarked
//!   `x!` is read as an unmarked `x` is (§17.8) — a borrow; assigning a borrow
//!   to an owned-state LHS is rejected — the mismatch would silently drop or
//!   double-hold a disposal obligation.
//! cites: 17.8, 28.3
//! expect: reject
//! diagnostic: owned-state
//! issue: 6609
module Conf;

public class Cell
  public value: int := 0;

  public constructor()
  end constructor;
end Cell;

public class Program
  public method Demo()
    var t: Cell? := new Cell();
    t := t!;              // `t!` reads as the bare read `t` does — a borrow,
                          // stored into an owned-state LHS — REJECT
    t!.Dispose();
  end Demo;
end Program;
