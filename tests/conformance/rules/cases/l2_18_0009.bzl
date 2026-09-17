//! case: l2_18_0009
//! doc: stdlib
//! section: 2.18
//! title: an unrecognized specifier raises FormatException
//! rule: The specifier vocabulary is invariant-culture only; an
//!   unrecognized specifier raises FormatException (strict-fault, as
//!   everywhere).
//! cites: 2.18
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    var n := 5;
    try
      Console.WriteLine($"{n:Q}");
    catch
      FormatException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
