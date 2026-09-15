//! case: s22_6_0004
//! doc: spec
//! section: 22.6
//! title: example 1 -- exact match preferred over a widening conversion
//! rule: "For a single argument position, one candidate is better than
//!   another when the better candidate requires no implicit conversion
//!   ... and the other requires an implicit conversion."
//! cites: 22.6
//! expect: run
//! stdout: int
module Conf;

public class Logger
  public constructor()
  end constructor;

  public method Log(level: int): string => "int";
  public method Log(level: long): string => "long";
end Logger;

public class Program
  public static method Main()
    var lg := new Logger();
    var n: int := 10;
    Console.WriteLine(lg.Log(n));
  end Main;
end Program;
