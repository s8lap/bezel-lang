//! case: s28_3_0217
//! doc: spec
//! section: 28.3
//! title: a self-assignment to a borrowed-state local is admitted
//! rule: State match on reassignment, by LHS state: a borrowed-state LHS
//!   requires a borrow (or `null`) RHS; an unmarked read of a named binding is a
//!   borrow, so the self-assignment of a borrowed-state local is admitted.
//! cites: 28.3
//! expect: accept
//! issue: 6609
module Conf;

public class Cell
  public value: int := 0;

  public constructor()
  end constructor;
end Cell;

public class Program
  public method Borrow(r: Cell)
    var b := r;     // `r` is a borrow, so `b` is a borrowed-state local
    b := b;         // the read of `b` is a borrow, and the LHS wants a borrow
  end Borrow;
end Program;
