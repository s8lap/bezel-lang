//! case: s7_8_0001
//! doc: spec
//! section: 7.8
//! title: null-forgiving on a non-null value yields the non-nullable form
//! rule: `x!` asserts non-nullness. The result is the non-nullable form of
//!   the expression.
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
    Console.WriteLine(w!.Name);
  end Main;
end Program;
