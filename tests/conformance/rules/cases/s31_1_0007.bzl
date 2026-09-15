//! case: s31_1_0007
//! doc: spec
//! section: 31.1
//! title: move is rejected as redundant on a throw operand
//! rule: `throw` is a marker-free consuming position — `throw x;` — the
//!   keyword itself is the announcement, so `throw move x;` is rejected at
//!   the marker-free position, the same no-marker rule as an `owned` return.
//! cites: 31.1, 28.3
//! expect: reject
//! diagnostic: redundant
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message)
  end constructor;
end BoomException;

public class Program
  public static method Main()
    var x := new BoomException("boom");
    throw move x;
  end Main;
end Program;
