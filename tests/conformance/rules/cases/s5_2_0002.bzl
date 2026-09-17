//! case: s5_2_0002
//! doc: spec
//! section: 5.2
//! title: a checked integer narrowing that does not fit raises OverflowException
//! rule: Integer narrowing raises OverflowException if the value does not fit.
//! cites: 5.2
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    try
      var n: int := 70000;
      var s := short(n);
      Console.WriteLine(s);
    catch
      var ex: OverflowException => Console.WriteLine("overflow");
    end;
  end Main;
end Program;
