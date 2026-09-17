//! case: s31_2_0004
//! doc: spec
//! section: 31.2
//! title: a more specific arm listed first catches its own type
//! rule: The arms are tried in order; the first arm whose pattern matches the
//!   thrown exception is selected. Reordering the arms of s31_2_0003 so the
//!   more specific type is first changes which arm is selected.
//! cites: 31.2
//! expect: run
//! output: narrow
module Conf;

public class BroadException: Exception
  public constructor(message: string) base(message)
  end constructor;
end BroadException;

public class NarrowException: BroadException
  public constructor(message: string) base(message)
  end constructor;
end NarrowException;

public class Program
  public static method Main()
    try
      throw new NarrowException("boom");
    catch
      NarrowException => Console.WriteLine("narrow"),
      BroadException => Console.WriteLine("broad"),
    end try;
  end Main;
end Program;
