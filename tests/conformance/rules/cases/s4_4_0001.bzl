//! case: s4_4_0001
//! doc: spec
//! section: 4.4
//! title: decimal division by zero raises DivideByZeroException
//! rule: Division by zero raises DivideByZeroException.
//! cites: 4.4
//! expect: run
//! stdout: caught
module Conf;

public class Program
  public static method Main()
    try
      var a := 1.0m;
      var b := 0.0m;
      var x := a / b;
      Console.WriteLine("no");
    catch
      var ex: DivideByZeroException => Console.WriteLine("caught");
    end try;
  end Main;
end Program;
