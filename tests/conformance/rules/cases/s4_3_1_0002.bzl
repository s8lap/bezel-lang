//! case: s4_3_1_0002
//! doc: spec
//! section: 4.3.1
//! title: negative zero equals positive zero
//! rule: -0.0 = +0.0 is true.
//! cites: 4.3.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{-0.0 = 0.0}");
  end Main;
end Program;
