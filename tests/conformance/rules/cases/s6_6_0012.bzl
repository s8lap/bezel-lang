//! case: s6_6_0012
//! doc: spec
//! section: 6.6
//! title: a non-constant shift count outside the domain raises OverflowException in <<=
//! rule: x <<= k and x >>= k inherit §6.5's count-domain rule through the
//!   desugaring: an out-of-domain count is the same fault — compile error for
//!   a constant count, OverflowException at runtime otherwise.
//! cites: 6.6, 6.5
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var k: int := 32;
      var v: int := 1;
      v <<= k;
      Console.WriteLine(v);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
