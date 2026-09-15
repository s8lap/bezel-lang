//! case: l5_0011
//! doc: stdlib
//! section: 5
//! title: GroupBy materializes fully on its first enumeration, one key-selector call per element
//! rule: GroupBy is lazy until its first enumeration, then materializes
//!   fully -- no complete group can exist before the source is exhausted,
//!   so the operator buffers.
//! cites: 5
//! expect: run
//! stdout: 4
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3, 4];
    var calls := 0;
    using var groups := xs.GroupBy(lambda(x: int): int => do
      calls := calls + 1;
      return x mod 2;
    end);
    for var g in groups do
    end;
    Console.WriteLine(calls);
  end Main;
end Program;
