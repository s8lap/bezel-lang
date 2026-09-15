//! case: s6_5_0005
//! doc: spec
//! section: 6.5
//! title: a non-constant shift count outside the domain raises OverflowException at runtime
//! rule: A count outside the domain ... raises OverflowException at runtime
//!   otherwise.
//! cites: 6.5
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var k: int := 64;
      var n := 1 << k;
      Console.WriteLine(n);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
