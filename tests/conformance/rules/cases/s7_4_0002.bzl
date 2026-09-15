//! case: s7_4_0002
//! doc: spec
//! section: 7.4
//! title: a null test narrows the true branch for member access
//! rule: A null test — `x <> null` — narrows the true branch, admitting
//!   member access there.
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
    if w <> null then
      Console.WriteLine(w.Name);
    end;
  end Main;
end Program;
