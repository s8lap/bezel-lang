//! case: s31_2_0011
//! doc: spec
//! section: 31.2
//! title: a throw escaping a finally supersedes the in-flight exception
//! rule: A throw escaping a `finally` supersedes the in-flight exception: when
//!   a `finally` entered during exception propagation itself completes by
//!   throwing, the new exception replaces the one being handled, and the
//!   superseding exception propagates from the `try` statement as usual.
//! cites: 31.2
//! expect: run
//! stdout: second
module Conf;

public class ExA: Exception
  public constructor(message: string) base(message);
  end constructor;
end ExA;

public class ExB: Exception
  public constructor(message: string) base(message);
  end constructor;
end ExB;

public class Program
  public static method Main()
    try
      try
        throw new ExA("first");
      finally
        throw new ExB("second");
      end try;
    catch
      var ex: Exception => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
