//! case: s31_2_4_0001
//! doc: spec
//! section: 31.2.4
//! title: the bare re-raise expression re-throws with the stack trace preserved
//! rule: The bare re-raise re-throws the currently-handled exception with its
//!   stack trace preserved. As an expression: `throw` (without a semicolon) —
//!   the natural form for an arm body that does nothing but re-raise.
//! cites: 31.2.4
//! expect: run
//! stdout: original
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message);
  end constructor;
end BoomException;

public class Program
  public static method Main()
    try
      try
        throw new BoomException("original");
      catch
        Exception => throw,
      end try;
    catch
      var outer: Exception => Console.WriteLine(outer.Message),
    end try;
  end Main;
end Program;
