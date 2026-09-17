//! case: s7_4_0004
//! doc: spec
//! section: 7.4
//! title: is not null narrows to the non-nullable form
//! rule: `x is not null` narrows to the non-nullable form.
//! cites: 12.7
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
    if w is not null then
      Console.WriteLine(w.Name);
    end;
  end Main;
end Program;
