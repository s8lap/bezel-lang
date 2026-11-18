//! case: s17_12_7201
//! doc: spec
//! section: 17.12
//! title: a throw-expression body is admitted at a value-returning function
//!   slot
//! rule: Both forms have the bottom type — assignable to any expected type
//!   — because control transfers to the matching handler before any value
//!   is produced.
//! cites: 17.12, 27.3
//! expect: run
//! stdout: before
module Conf;

public class Program
  public static method Main()
    var f: () -> int := lambda() => throw new Exception("x");
    Console.WriteLine("before");
  end Main;
end Program;
