//! case: s16_1_0001
//! doc: spec
//! section: 16.1
//! title: an inclusive range includes its upper bound
//! rule: "`a..b` is inclusive on both ends" — iterating `1..10` yields 10 as its
//!   last value.
//! cites: 16.1, 16.5
//! expect: run
//! stdout: 10
module Conf;

public class Program
  public static method Main()
    var last: int := 0;
    for var i in 1..10 do
      last := i;
    end for;
    Console.WriteLine(last);
  end Main;
end Program;
