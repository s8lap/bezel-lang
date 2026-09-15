//! case: s31_3_0002
//! doc: spec
//! section: 31.3
//! title: an exception propagates undeclared through several call frames
//! rule: Any exception may propagate from any method call; none of the
//!   intermediate methods in the chain need declare it.
//! cites: 31.3
//! expect: run
//! stdout: deep failure
module Conf;

public class Program
  public static method Level3()
    throw new InvalidOperationException("deep failure");
  end Level3;

  public static method Level2()
    Level3();
  end Level2;

  public static method Level1()
    Level2();
  end Level1;

  public static method Main()
    try
      Level1();
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
