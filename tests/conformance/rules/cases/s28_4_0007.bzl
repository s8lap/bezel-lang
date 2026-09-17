//! case: s28_4_0007
//! doc: spec
//! section: 28.4
//! title: a throwing disposer does not skip the remaining disposals in the same cleanup pass
//! rule: In a multi-value cleanup pass, a throwing disposer does not skip the remaining
//!   values: every remaining disposal still runs, and the exception then propagates
//!   from the disposal point.
//! expect: run
//! output: dispose w
//!   caught
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("dispose w");
  end destructor;
end Widget;

public class Faulty
  public constructor()
  end constructor;

  destructor
    throw new InvalidOperationException("boom");
  end destructor;
end Faulty;

public class Container
  private owned _w: Widget;
  private owned _f: Faulty;

  public constructor()
    _w := new Widget();
    _f := new Faulty();
  end constructor;
end Container;

public class Program
  public static method Main()
    var c := new Container();
    try
      c.Dispose();
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Program;
