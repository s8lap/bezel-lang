//! case: s28_5_0017
//! doc: spec
//! section: 28.5
//! title: owned fields are disposed after the destructor body runs
//! rule: The `owned` modifier auto-disposes the field when the containing
//!   object is disposed, after the destructor body runs.
//! cites: 28.4, 28.5
//! expect: run
//! stdout: container destructor
module Conf;

public class Widget
  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public class Container
  private owned _w: Widget;

  public constructor()
    _w := new Widget();
  end constructor;

  destructor
    Console.WriteLine("container destructor");
  end destructor;
end Container;

public class Program
  public static method Main()
    using var c := new Container();
  end Main;
end Program;
