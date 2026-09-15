//! case: s4_3_1_0001
//! doc: spec
//! section: 4.3.1
//! title: NaN equals NaN under reflexive equality
//! rule: NaN = NaN is true.
//! cites: 4.3.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{double.NaN = double.NaN}");
  end Main;
end Program;
