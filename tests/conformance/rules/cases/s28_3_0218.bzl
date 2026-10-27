//! case: s28_3_0218
//! doc: spec
//! section: 28.3
//! title: a self-assignment to an interface-typed owned local is rejected
//! rule: Assigning a borrow to an owned-state LHS, or an owned value to a
//!   borrowed-state LHS, is rejected — the mismatch would silently drop or
//!   double-hold a disposal obligation.
//! cites: 28.3
//! expect: reject
//! diagnostic: owned-state
//! issue: 6609
module Conf;

public interface I : IDisposable
end I;

public class Cell : I
  public value: int := 0;

  public constructor()
  end constructor;
end Cell;

public class Program
  public method Demo()
    var i: I := new Cell();   // the interface-typed local holds an owned value
    i := i;                   // the unmarked read of `i` is a borrow, stored into
                              // an owned-state LHS — REJECT
    i.Dispose();
  end Demo;
end Program;
