//! case: s14_5_0022
//! doc: spec
//! section: 14.5
//! title: an explicit long list admits a uint and an int argument that agree on no type of their own
//! rule: An explicit type-argument list (obj.Method<int>(args), §22.12/A.7) binds
//!   the declared type parameters in declaration order and must supply exactly
//!   as many arguments as the method declares type parameters. When a full
//!   explicit list is supplied, no inference runs.
//! cites: 14.5, 5.1
//! expect: run
//! output: 4000000000
//!   7
module Conf;

public class Program
  public static method Main()
    var u: uint := 4000000000;
    var i: int := 7;
    Console.WriteLine(Math.Max<long>(u, i));
    Console.WriteLine(Math.Min<long>(u, i));
  end Main;
end Program;
