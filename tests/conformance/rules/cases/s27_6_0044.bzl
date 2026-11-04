//! case: s27_6_0044
//! doc: spec
//! section: 27.6
//! title: a bound reference to a zero-parameter extension method is refused against a one-parameter target
//! rule: The reference's signature must be compatible with the target function
//!   type: its parameter and return types unify with the target's under the
//!   standard implicit-conversion rules
//! cites: 27.6
//! expect: reject
//! diagnostic: not implicitly convertible
//! issue: 7488
module Conf;

extension Array<int>
  public method Length(): int => 42;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var f: (int) -> int := a.Length;
  end Main;
end Program;
