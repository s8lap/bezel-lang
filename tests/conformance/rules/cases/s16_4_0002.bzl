//! case: s16_4_0002
//! doc: spec
//! section: 16.4
//! title: a runtime-computed step of zero raises OverflowException at the range expression
//! rule: "evaluating a range expression whose computed step is zero raises
//!   `OverflowException` at the expression."
//! cites: 16.4
//! expect: run
//! stdout: overflow
module Conf;

public class Program
  public static method Main()
    var s: long := 0;
    try
      var r := 5..10 step s;
      Console.WriteLine(r.Step);
    catch
      var ex: OverflowException => Console.WriteLine("overflow")
    end try;
  end Main;
end Program;
