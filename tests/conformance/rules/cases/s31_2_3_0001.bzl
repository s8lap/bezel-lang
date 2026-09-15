//! case: s31_2_3_0001
//! doc: spec
//! section: 31.2.3
//! title: a specific arm before the catch-all handles its own type
//! rule: Arms are tried in order; the first match wins. The most specific
//!   exception types should appear first, ahead of a bare `Exception` catch-all.
//! cites: 31.2.3
//! expect: run
//! output: retry
module Conf;

public class TimeoutLikeException: Exception
  public constructor(message: string) base(message)
  end constructor;
end TimeoutLikeException;

public class Program
  public static method Main()
    try
      throw new TimeoutLikeException("slow");
    catch
      var ex: TimeoutLikeException => Console.WriteLine("retry"),
      var ex: Exception => Console.WriteLine("catch-all"),
    end try;
  end Main;
end Program;
