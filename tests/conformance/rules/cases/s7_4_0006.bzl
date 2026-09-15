//! case: s7_4_0006
//! doc: spec
//! section: 7.4
//! title: copying a mutable field to a local admits narrowing on the copy
//! rule: The standard pattern is to copy a nullable field to a local before
//!   narrowing; local-variable narrowing then applies to the copy.
//! expect: run
//! stdout: gizmo
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
    var local := _w;
    if local <> null then
      Console.WriteLine(local.Name);
    end;
  end Report;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    h.Report();
  end Main;
end Program;
