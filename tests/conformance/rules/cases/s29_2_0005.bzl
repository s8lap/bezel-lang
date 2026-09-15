//! case: s29_2_0005
//! doc: spec
//! section: 29.2
//! title: an element borrow escaping the loop is rejected over an interface-typed source
//! rule: Over an **interface-typed** source the compiler cannot see the
//!   backing, so the variable's source is conservatively the loop's own
//!   enumerator — a scope-bound local — and any escape is rejected.
//! cites: 29.2
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
  public static method FindByName(items: IEnumerable<Widget>, target: string): Widget?
    var found: Widget? := null;
    for var w in items do
      if w.Name = target then
        found := w;
        break;
      end;
    end;
    return found;
  end FindByName;
end Program;
