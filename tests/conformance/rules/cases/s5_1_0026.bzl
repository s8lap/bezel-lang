//! case: s5_1_0026
//! doc: spec
//! section: 5.1
//! title: a string argument to Convert's string parameter is admitted and converted
//! rule: stdlib §2.3 declares `ToInt32(value: string): int`. §5.1's identity
//!   reference conversion - every type converts to itself - makes the `string`
//!   parameter match exactly, so the call is admitted and §2.3's conversion
//!   produces the int 42. This is the accepting counterpart of the `int` argument
//!   the conversion table does not reach.
//! cites: 5.1, 2.3
//! expect: run
//! stdout: 42
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Convert.ToInt32("42"));
  end Main;
end Program;
