//! case: s7_4_0007
//! doc: spec
//! section: 7.4
//! title: reassignment from a nullable expression invalidates narrowing
//! rule: The narrowing of a variable is invalidated when, within the
//!   controlled block, the variable is reassigned from a nullable-typed
//!   expression.
//! expect: reject
//! diagnostic: reassigned
module Conf;

public class Widget
  public constructor()
  end constructor;

  public readonly property Name: string := "gizmo";
end Widget;

public class Program
  public static method Describe(w: Widget?): string
    if w <> null then
      w := null;
      return w.Name;
    end;
    return "absent";
  end Describe;
end Program;
