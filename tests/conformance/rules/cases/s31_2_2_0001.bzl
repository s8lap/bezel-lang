//! case: s31_2_2_0001
//! doc: spec
//! section: 31.2.2
//! title: an expression-form catch arm body discards its result
//! rule: A catch arm's body may be an expression: `=> SomeMethod(args)`; the
//!   expression's result is discarded.
//! cites: 31.2.2
//! expect: run
//! stdout: logged
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message);
  end constructor;
end BoomException;

public class Program
  public static method Log(): int
    Console.WriteLine("logged");
    return 0;
  end Log;

  public static method Main()
    try
      throw new BoomException("x");
    catch
      BoomException => Log(),
    end try;
  end Main;
end Program;
