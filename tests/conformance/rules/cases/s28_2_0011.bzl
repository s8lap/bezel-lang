//! case: s28_2_0011
//! doc: spec
//! section: 28.2
//! title: the default build is checked: a read through a surviving borrow after Dispose faults
//! rule: Checked mode — any instance member access on a logically-disposed
//!   instance — an instance method call, a property or indexer accessor
//!   invocation, or a field access through the reference — raises
//!   `DisposedAccessException`. This is the default: a build that selects
//!   neither mode is a checked build.
//! cites: 28.2, 31.4, 28.3
//! expect: run
//! stdout: faulted
//! issue: 4602
module Conf;

public class Widget
  public property Label: string;

  public constructor()
    Label := "live";
  end constructor;

  destructor
  end destructor;
end Widget;

public class Program
  public static method Main()
    var w := new Widget();
    var view := w;
    w.Dispose();
    try
      Console.WriteLine(view.Label);
    catch
      DisposedAccessException => Console.WriteLine("faulted"),
    end try;
  end Main;
end Program;
