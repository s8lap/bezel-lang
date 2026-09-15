//! case: s31_2_1_0001
//! doc: spec
//! section: 31.2.1
//! title: a bare type arm matches without binding
//! rule: A catch arm's pattern may be a bare type, `IOException`: it matches
//!   any thrown exception that is an instance of the type, without binding.
//! cites: 31.2.1
//! expect: run
//! stdout: caught
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message)
  end constructor;
end BoomException;

public class Program
  public static method Main()
    try
      throw new BoomException("x");
    catch
      BoomException => Console.WriteLine("caught"),
    end try;
  end Main;
end Program;
