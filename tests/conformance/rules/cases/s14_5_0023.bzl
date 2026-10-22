//! case: s14_5_0023
//! doc: spec
//! section: 14.5
//! title: an explicit double list types the call's result as double for two int arguments
//! rule: An explicit type-argument list (obj.Method<int>(args), §22.12/A.7) binds
//!   the declared type parameters in declaration order.
//! cites: 14.5, 5.1
//! expect: run
//! stdout: 2.5
module Conf;

public class Program
  public static method Main()
    var i: int := 3;
    var j: int := 5;
    Console.WriteLine(Math.Max<double>(i, j) / 2);
  end Main;
end Program;
