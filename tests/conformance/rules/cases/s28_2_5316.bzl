//! case: s28_2_5316
//! doc: spec
//! section: 28.2
//! title: an explicit Dispose of a using var Random runs once and the scope end does not free it again
//! rule: The two modes give identical observable behavior for every program that never uses a disposed
//!   instance.
//! cites: 28.2, 28.10, 28.6
//! expect: run
//! output: 200
//! flags: --checked-disposal
module Conf;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<200 do
      using var c := new Random(1);
      total := total + 1;
      c.Dispose();
    end;
    Console.WriteLine(total);
  end Main;
end Program;
