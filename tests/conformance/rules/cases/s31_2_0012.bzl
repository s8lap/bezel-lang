//! case: s31_2_0012
//! doc: spec
//! section: 31.2
//! title: the superseding exception's SupersededChain records the lost exception
//! rule: At the instant of supersession, and before the superseded exception
//!   is disposed, the runtime appends to the superseding exception's
//!   `SupersededChain` an `ExceptionInfo` snapshot of the superseded
//!   exception itself — its type name, `Message`, and `StackTrace`.
//! cites: 31.2, 31.4
//! expect: run
//! stdout: first
module Conf;

public class ExA: Exception
  public constructor(message: string) base(message)
  end constructor;
end ExA;

public class ExB: Exception
  public constructor(message: string) base(message)
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
      var ex: Exception => Console.WriteLine(ex.SupersededChain[0].Message),
    end try;
  end Main;
end Program;
