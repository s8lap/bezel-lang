//! case: s5_2_0004
//! doc: spec
//! section: 5.2
//! title: casting a negative int to uint raises OverflowException
//! rule: A conversion between a signed and unsigned integer type of the same
//!   width is a checked narrowing: it raises OverflowException when the value
//!   is not representable in the destination type — a negative value cast to
//!   an unsigned type.
//! cites: 5.2
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var n: int := -1;
      var u := uint(n);
      Console.WriteLine(u);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
