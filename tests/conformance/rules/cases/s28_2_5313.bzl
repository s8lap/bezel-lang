//! case: s28_2_5313
//! doc: spec
//! section: 28.2
//! title: a using var Set<long> released only at scope end is used only while live
//! rule: The two modes give identical observable behavior for every program that never uses a disposed
//!   instance.
//! cites: 28.2, 28.10
//! expect: run
//! output: 200
//! flags: --checked-disposal
module Conf;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<200 do
      using var x := new Set<long>();
      x.Add(i);
      total := total + x.Count;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
