//! case: s7_5_0001
//! doc: spec
//! section: 7.5
//! title: null-conditional access performs the access when the receiver is non-null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the
//!   access. The result type is always nullable.
//! expect: run
//! stdout: gizmo
module Conf;

public class Widget
  public constructor()
  end constructor;

  public readonly property Name: string := "gizmo";
end Widget;

public class Program
  public static method Main()
    var w: Widget? := new Widget();
    var n: string? := w?.Name;
    Console.WriteLine($"{n}");
  end Main;
end Program;
