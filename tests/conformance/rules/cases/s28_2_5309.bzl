//! case: s28_2_5309
//! doc: spec
//! section: 28.2
//! title: a Set borrow read before the owner's explicit Dispose never touches the disposed set
//! rule: The two modes give identical observable behavior for every program that never uses a disposed
//!   instance.
//! cites: 28.2, 28.3.9
//! expect: run
//! output: 200
//! flags: --checked-disposal
module Conf;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<200 do
      var x := new Set<int>();
      x.Add(i);
      var b := x;
      total := total + b.Count;
      x.Dispose();
    end;
    Console.WriteLine(total);
  end Main;
end Program;
