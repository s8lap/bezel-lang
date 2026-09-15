//! case: s16_1_0002
//! doc: spec
//! section: 16.1
//! title: a half-open range excludes its upper bound
//! rule: "`a..<b` is inclusive on the lower bound, exclusive on the upper" —
//!   iterating `0..<10` yields 9, never 10, as its last value.
//! cites: 16.1, 16.5
//! expect: run
//! stdout: 9
module Conf;

public class Program
  public static method Main()
    var last: int := 0;
    for var i in 0..<10 do
      last := i;
    end for;
    Console.WriteLine(last);
  end Main;
end Program;
