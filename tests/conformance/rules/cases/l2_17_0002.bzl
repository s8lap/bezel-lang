//! case: l2_17_0002
//! doc: stdlib
//! section: 2.17
//! title: every subsequent access to a failed type raises the same exception
//! rule: Raised on access to a static member of a type whose static
//!   initialization failed — every subsequent access raises
//!   TypeInitializationException, not just the first.
//! cites: 2.17, 22.13
//! expect: run
//! stdout: second
module Conf;

public class Boom: Exception
  public constructor(message: string) base(message)
  end constructor;
end Boom;

public class Config
  public static readonly property Value: int := ThrowIt();

  public static method ThrowIt(): int
    throw new Boom("boom");
  end ThrowIt;
end Config;

public class Program
  public static method Main()
    try
      _ := Config.Value;
    catch
      TypeInitializationException => Console.Write("")
    end;
    try
      _ := Config.Value;
    catch
      TypeInitializationException => Console.WriteLine("second")
    end;
  end Main;
end Program;
