//! case: s22_13_0007
//! doc: spec
//! section: 22.13
//! title: TypeInitializationException carries the original exception as InnerException
//! rule: A touch of a type whose static initialization is in progress
//!   raises TypeInitializationException; more generally, every
//!   subsequent access to a static member of a permanently-failed type
//!   raises TypeInitializationException whose InnerException is the
//!   original exception.
//! cites: 22.13, 22.7
//! expect: run
//! stdout: True
module Conf;

public class Config
  private static _v: int := Boom();

  private static method Boom(): int
    throw new InvalidOperationException("boom");
  end Boom;

  public static method Touch()
    var x := _v;
  end Touch;
end Config;

public class Program
  public static method Main()
    try
      Config.Touch();
    catch
      var ex: TypeInitializationException => Console.WriteLine(ex.InnerException <> null),
    end;
  end Main;
end Program;
