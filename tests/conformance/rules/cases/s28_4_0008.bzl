//! case: s28_4_0008
//! doc: spec
//! section: 28.4
//! title: the destructor body runs on disposal, never on construction unwind
//! rule: A constructor that throws unwinds instead: already-completed owned fields are
//!   disposed, and the throwing class's own destructor body is not run.
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

public class Faulty
  private owned _w: Widget;

  public constructor()
    _w := new Widget();
    throw new InvalidOperationException("boom");
  end constructor;

  destructor
    Console.WriteLine("faulty destructor ran");
  end destructor;
end Faulty;

public class Program
  public static method Main()
    try
      var f := new Faulty();
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Program;
