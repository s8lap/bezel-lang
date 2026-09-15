//! case: s22_13_0008
//! doc: spec
//! section: 22.13
//! title: every access to a permanently-failed type observes the same cached TypeInitializationException
//! rule: The instance constructed on first failure is the one cached and
//!   rethrown; the triggering access and every subsequent access see the
//!   same cached TypeInitializationException instance.
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
      var ex1: TypeInitializationException => do
        try
          Config.Touch();
        catch
          var ex2: TypeInitializationException => Console.WriteLine(ex1 = ex2),
        end;
      end,
    end;
  end Main;
end Program;
