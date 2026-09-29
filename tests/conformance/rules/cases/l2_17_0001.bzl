//! case: l2_17_0001
//! doc: stdlib
//! section: 2.17
//! title: TypeInitializationException.InnerException carries the original
//! rule: TypeInitializationException.InnerException carries the original
//!   exception that escaped the static initializer.
//! cites: 2.17, 22.13
//! expect: run
//! stdout: boom
module Conf;

public class Boom: Exception
  public constructor(message: string) base(message);
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
      var ex: TypeInitializationException => Console.WriteLine(ex.InnerException!.Message)
    end;
  end Main;
end Program;
