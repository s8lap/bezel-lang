//! case: s28_4_0001
//! doc: spec
//! section: 28.4
//! title: the generated Dispose runs the destructor body, then owned fields, then the base chain
//! rule: The compiler generates a public, idempotent `Dispose()` that runs the
//!   destructor body, disposes owned-bearing fields in reverse declaration order,
//!   then chains to the base class's `Dispose()`.
//! expect: run
//! stdout: derived disposed
module Conf;

public class Widget
  destructor
    Console.WriteLine("field disposed");
  end destructor;
end Widget;

public class Base
  destructor
    Console.WriteLine("base disposed");
  end destructor;
end Base;

public class Derived: Base
  private owned _w: Widget;

  public constructor()
    _w := new Widget();
  end constructor;

  destructor
    Console.WriteLine("derived disposed");
  end destructor;
end Derived;

public class Program
  public static method Main()
    using var d := new Derived();
  end Main;
end Program;
