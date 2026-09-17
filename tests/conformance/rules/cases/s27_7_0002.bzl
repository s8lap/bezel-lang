//! case: s27_7_0002
//! doc: spec
//! section: 27.7
//! title: a null-conditional call on a null function value is a no-op
//! rule: `maybeHandler?(42);` is a null-conditional call — a no-op when the
//!   function value is null.
//! cites: 27.7
//! expect: run
//! stdout: done
module Conf;

public class Program
  public static method Main()
    var maybeHandler: ((int) -> ())? := null;
    maybeHandler?(42);
    Console.WriteLine("done");
  end Main;
end Program;
