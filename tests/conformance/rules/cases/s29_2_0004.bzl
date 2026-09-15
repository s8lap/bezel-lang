//! case: s29_2_0004
//! doc: spec
//! section: 29.2
//! title: an element borrow escaping the loop is admitted over a concrete collection type
//! rule: Over a **concrete** collection type the chain is slot → collection →
//!   binding, and keeping the reference past the loop is fine while the
//!   collection lives.
//! cites: 29.2
//! expect: run
//! stdout: b
module Conf;

public class Widget
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Widget;

public class Program
  public static method FindByName(items: Array<Widget>, target: string): Widget?
    var found: Widget? := null;
    for var w in items do
      if w.Name = target then
        found := w;
        break;
      end;
    end;
    return found;
  end FindByName;

  public static method Main()
    var widgets: Array<Widget> := [new Widget("a"), new Widget("b")];
    var found := FindByName(widgets, "b");
    Console.WriteLine(found!.Name);
  end Main;
end Program;
