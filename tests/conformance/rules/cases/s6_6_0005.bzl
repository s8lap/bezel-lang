//! case: s6_6_0005
//! doc: spec
//! section: 6.6
//! title: a compound assignment evaluates each constituent of its left-hand side exactly once
//! rule: Every constituent of the left-hand side's lvalue chain is evaluated
//!   exactly once, left to right — including every index expression in the
//!   chain.
//! cites: 6.6, 17.6
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    var arr: Array<int> := [10, 20, 30];
    var calls: int := 0;
    var idx := lambda() => do calls := calls + 1; return 0; end;
    arr[idx()] += 1;
    Console.WriteLine(calls);
  end Main;
end Program;
