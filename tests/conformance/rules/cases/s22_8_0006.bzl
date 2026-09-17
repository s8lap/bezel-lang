//! case: s22_8_0006
//! doc: spec
//! section: 22.8
//! title: a readonly assignment inside a loop body is rejected unconditionally
//! rule: An assignment lexically inside a loop body within the assignment
//!   window is rejected unconditionally: a loop could assign on more than
//!   one iteration.
//! cites: 22.8
//! expect: reject
//! diagnostic: loop
module Conf;

public class Counter
  private readonly _start: int;

  public constructor(n: int)
    for var i in 0..<1 do
      _start := i;
    end for;
  end constructor;
end Counter;
