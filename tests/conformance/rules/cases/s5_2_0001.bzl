//! case: s5_2_0001
//! doc: spec
//! section: 5.2
//! title: explicit int-from-double conversion truncates toward zero
//! rule: var i: int := int(d); // truncates toward zero.
//! cites: 5.2
//! expect: run
//! stdout: -3
module Conf;

public class Program
  public static method Main()
    var d: double := -3.9;
    var n := int(d);
    Console.WriteLine(n);
  end Main;
end Program;
