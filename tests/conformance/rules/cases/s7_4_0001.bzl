//! case: s7_4_0001
//! doc: spec
//! section: 7.4
//! title: member access on an unnarrowed nullable value is rejected
//! rule: Member access, method calls, and indexing on a nullable value are
//!   compile errors unless the compiler has narrowed it to non-null.
//! expect: reject
//! diagnostic: narrowed
module Conf;

public class Widget
  public constructor()
  end constructor;

  public readonly property Name: string := "gizmo";
end Widget;

public class Program
  public static method Describe(w: Widget?): string => w.Name;
end Program;
