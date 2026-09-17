//! case: s7_4_0008
//! doc: spec
//! section: 7.4
//! title: passing a narrowed local as a ref argument invalidates narrowing
//! rule: The narrowing of a variable is invalidated when, within the
//!   controlled block, the variable is passed as a `ref` or `out` argument
//!   (the callee may reassign it).
//! cites: 22.3
//! expect: reject
//! diagnostic: passed as a ref
module Conf;

public class Widget
  public constructor()
  end constructor;

  public readonly property Name: string := "gizmo";
end Widget;

public class Program
  public static method Reset(ref w: Widget?) => w := null;

  public static method Describe(w: Widget?)
    if w <> null then
      Reset(ref w);
      Console.WriteLine(w.Name);
    end;
  end Describe;
end Program;
