//! case: s7_9_0003
//! doc: spec
//! section: 7.9
//! title: a field initializer at declaration satisfies definite assignment
//! rule: Every non-nullable field must be given a field initializer at its
//!   declaration, as an alternative to constructor assignment.
//! expect: run
//! stdout: gizmo
module Conf;

public class Widget
  public constructor()
  end constructor;

  public readonly property Name: string := "gizmo";
end Widget;

public class Holder
  private owned _w: Widget := new Widget();

  public constructor()
  end constructor;

  public method Report() => Console.WriteLine(_w.Name);
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    h.Report();
  end Main;
end Program;
