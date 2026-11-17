//! case: s28_2_5302
//! doc: spec
//! section: 28.2
//! title: an explicit Dispose of a plain var Set is observed once, with no false use-after-dispose
//! rule: The two modes give identical observable behavior for every program that never uses a disposed
//!   instance.
//! cites: 28.2, 28.6
//! expect: run
//! output: 200
//! flags: --checked-disposal
module Conf;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<200 do
      var c := new Set<int>();
      c.Add(i);
      total := total + c.Count;
      c.Dispose();
    end;
    Console.WriteLine(total);
  end Main;
end Program;
