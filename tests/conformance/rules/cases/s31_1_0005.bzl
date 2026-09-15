//! case: s31_1_0005
//! doc: spec
//! section: 31.1
//! title: throwing the caught exception binding directly is rejected
//! rule: The throw operand is consumed as an owned value, so a borrowed class
//!   reference is not a valid operand; to re-raise the exception being
//!   handled, use the bare re-raise instead.
//! cites: 31.1, 31.2.4
//! expect: reject
//! diagnostic: owned
module Conf;

public class InnerException: Exception
  public constructor(message: string) base(message)
  end constructor;
end InnerException;

public class Program
  public static method Main()
    try
      try
        throw new InnerException("inner");
      catch
        var ex: InnerException => throw ex,
      end try;
    catch
      var outer: Exception => Console.WriteLine(outer.Message),
    end try;
  end Main;
end Program;
