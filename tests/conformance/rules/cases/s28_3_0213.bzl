//! case: s28_3_0213
//! doc: spec
//! section: 28.3
//! title: an owned-state local accepts a marked transfer of its own binding
//! rule: State match on reassignment, by LHS state: an owned-state LHS requires
//!   an owned RHS — a fresh producer, or a named owned binding under the explicit
//!   transfer marker (`x := move y;`) — and disposes the displaced value per
//!   drop-on-overwrite (§28.7).
//! cites: 28.3, 28.7
//! expect: accept
//! issue: 6609
module Conf;

public class Cell
  public value: int := 0;

  public constructor()
  end constructor;
end Cell;

public class Program
  public method Demo()
    var t := new Cell();
    t := move t;     // the marked transfer makes the RHS an owned value
    t.Dispose();
  end Demo;
end Program;
