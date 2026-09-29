//! case: s31_2_0010
//! doc: spec
//! section: 31.2
//! title: an owned local of the protected block is disposed before finally runs
//! rule: The `finally` is established at entry to the protected block: on the
//!   single establishment-ordered cleanup stack it ranks before anything the
//!   block itself establishes, so an owned local or `using var` declared
//!   inside the protected block is disposed before the `finally` runs on
//!   every exit edge.
//! cites: 31.2, 28.10
//! expect: run
//! stdout: widget disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public class BoomException: Exception
  public constructor(message: string) base(message);
  end constructor;
end BoomException;

public class Program
  public static method Main()
    try
      using var w := new Widget();
      throw new BoomException("boom");
    catch
      BoomException => Console.WriteLine("caught"),
    finally
      Console.WriteLine("finally");
    end try;
  end Main;
end Program;
