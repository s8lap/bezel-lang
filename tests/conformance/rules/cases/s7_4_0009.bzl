//! case: s7_4_0009
//! doc: spec
//! section: 7.4
//! title: a nullable readonly field narrows across an intervening call
//! rule: A nullable `readonly` field participates in flow narrowing, because
//!   outside its construction window it has no writer on any path, through
//!   any alias, so no call can invalidate a fact about its value.
//! expect: run
//! stdout: gizmo
module Conf;

public class Widget
  public constructor()
  end constructor;

  public readonly property Name: string := "gizmo";
end Widget;

public class Holder
  private owned readonly _w: Widget?;

  public constructor()
    _w := new Widget();
  end constructor;

  public method Noisy()
  end Noisy;

  public method Report()
    if _w <> null then
      Noisy();
      Console.WriteLine(_w.Name);
    end;
  end Report;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    h.Report();
  end Main;
end Program;
