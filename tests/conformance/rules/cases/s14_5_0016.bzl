//! case: s14_5_0016
//! doc: spec
//! section: 14.5
//! title: a full explicit type-argument list binds T, so the generic body computes at the bound type
//! rule: An explicit type-argument list (obj.Method<int>(args), §22.12/A.7) binds
//!   the declared type parameters in declaration order and must supply exactly
//!   as many arguments as the method declares type parameters. When a full
//!   explicit list is supplied, no inference runs.
//! cites: 14.5, 5.1
//! expect: run
//! output: 4000000000
//!   0.75
module Conf;

public static class Util
  public static method Add<T>(a: T, b: T): T where T: INumber<T> => a + b;
end Util;

public class Program
  public static method Main()
    Console.WriteLine(Util.Add<long>(2000000000, 2000000000));
    Console.WriteLine(Util.Add<decimal>(1, 2) / 4m);
  end Main;
end Program;
