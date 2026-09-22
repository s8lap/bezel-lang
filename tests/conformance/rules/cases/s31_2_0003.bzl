//! case: s31_2_0003
//! doc: spec
//! section: 31.2
//! title: a broader arm listed first catches a more specific exception
//! rule: The arms are tried in order; the first arm whose pattern matches the
//!   thrown exception is selected, its body runs, and exception handling
//!   completes. Arms after the matched one are not considered.
//! cites: 31.2
//! expect: run
//! output: broad
module Conf;

public class BroadException: Exception
  public constructor(message: string) base(message);
  end constructor;
end BroadException;

public class NarrowException: BroadException
  public constructor(message: string) base(message);
  end constructor;
end NarrowException;

public class Program
  public static method Main()
    try
      throw new NarrowException("boom");
    catch
      BroadException => Console.WriteLine("broad"),
      NarrowException => Console.WriteLine("narrow"),
    end try;
  end Main;
end Program;
