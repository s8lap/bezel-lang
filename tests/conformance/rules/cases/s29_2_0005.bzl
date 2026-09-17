//! case: s29_2_0005
//! doc: spec
//! section: 29.2
//! title: an element borrow escaping the loop is rejected over an interface-typed source
//! rule: At an interface-typed enumeration source the backing is unprovable, so
//!   the iteration variable's source is conservatively the loop's implicit
//!   enumerator, a scope-bound local, and letting it escape the loop is
//!   rejected.
//! cites: 29.2, 28.3
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Widget
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Widget;

public class Program
  public static method Main()
    var widgets: Array<Widget> := [new Widget("a"), new Widget("b")];
    var seq: IEnumerable<Widget> := widgets;
    var found: Widget? := null;
    for var w in seq do
      if w.Name = "b" then
        found := w;
        break;
      end;
    end;
    Console.WriteLine(found!.Name);
  end Main;
end Program;
