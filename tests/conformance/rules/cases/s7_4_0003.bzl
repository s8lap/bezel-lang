//! case: s7_4_0003
//! doc: spec
//! section: 7.4
//! title: an early exit on the null branch narrows the surviving path
//! rule: An early exit — after `if x = null then return; end;` — narrows the
//!   surviving path.
//! expect: run
//! stdout: gizmo
module Conf;

public class Widget
  public constructor()
  end constructor;

  public readonly property Name: string := "gizmo";
end Widget;

public class Program
  public static method Describe(w: Widget?): string
    if w = null then
      return "absent";
    end;
    return w.Name;
  end Describe;

  public static method Main()
    Console.WriteLine(Describe(new Widget()));
  end Main;
end Program;
