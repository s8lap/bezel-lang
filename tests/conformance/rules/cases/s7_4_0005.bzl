//! case: s7_4_0005
//! doc: spec
//! section: 7.4
//! title: a mutable field never narrows, even after a null check
//! rule: The compiler narrows local variables, parameters other than `ref`
//!   parameters, and `readonly` fields in the tracked shape below; these are
//!   the narrowing carriers. It never narrows mutable fields, properties, or
//!   `ref` parameters.
//! expect: reject
//! diagnostic: never narrow
module Conf;

public class Widget
  public constructor()
  end constructor;

  public readonly property Name: string := "gizmo";
end Widget;

public class Holder
  private owned _w: Widget?;

  public constructor()
    _w := new Widget();
  end constructor;

  public method Report()
    if _w <> null then
      Console.WriteLine(_w.Name);
    end;
  end Report;
end Holder;
