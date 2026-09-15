//! case: s31_2_3_0002
//! doc: spec
//! section: 31.2.3
//! title: a bare Exception arm at the end is the catch-all
//! rule: A bare `Exception` arm at the end is the catch-all: every throwable
//!   derives from `Exception`, so such an arm captures any thrown exception.
//! cites: 31.2.3
//! expect: run
//! stdout: catch-all
module Conf;

public class TimeoutLikeException: Exception
  public constructor(message: string) base(message)
  end constructor;
end TimeoutLikeException;

public class Program
  public static method Main()
    try
      throw new ArgumentException("bad arg");
    catch
      var ex: TimeoutLikeException => Console.WriteLine("retry"),
      var ex: Exception => Console.WriteLine("catch-all"),
    end try;
  end Main;
end Program;
