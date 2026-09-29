//! case: s31_1_0006
//! doc: spec
//! section: 31.1
//! title: the bare re-raise is the admitted way to re-throw a caught exception
//! rule: To re-raise the exception being handled, use the bare re-raise
//!   (`throw`, valid only inside a catch arm body) rather than throwing the
//!   borrowed catch-arm binding.
//! cites: 31.1, 31.2.4
//! expect: run
//! stdout: inner
module Conf;

public class InnerException: Exception
  public constructor(message: string) base(message);
  end constructor;
end InnerException;

public class Program
  public static method Main()
    try
      try
        throw new InnerException("inner");
      catch
        var ex: InnerException => throw,
      end try;
    catch
      var outer: Exception => Console.WriteLine(outer.Message),
    end try;
  end Main;
end Program;
