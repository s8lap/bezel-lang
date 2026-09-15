//! case: s29_2_0004
//! doc: spec
//! section: 29.2
//! title: an element borrow escaping the loop is admitted over a concrete collection type
//! rule: An iteration-variable borrow is admissible where its derived source
//!   chain reaches the collection under §29.2's desugaring: a collection-backed
//!   source re-borrows — the chain runs element slot → collection — so the
//!   reference stays valid past the loop while the collection lives.
//! cites: 29.2, 28.3, 28.11
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
  public static method Main()
    var widgets: Array<Widget> := [new Widget("a"), new Widget("b")];
    var found: Widget? := null;
    for var w in widgets do
      if w.Name = "b" then
        found := w;
        break;
      end;
    end;
    Console.WriteLine(found!.Name);
  end Main;
end Program;
