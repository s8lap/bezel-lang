//! case: l4_21_0004
//! doc: stdlib
//! section: 4.21
//! title: Clear disposes every value in descending Index and keeps the map usable
//! rule: Clear() disposes every value in descending Index and keeps the
//!   emptied map.
//! cites: 4.21, 28.11
//! expect: run
//! output: disposed c
//!   disposed b
//!   disposed a
//!   cleared 0
//!   disposed d
//! issue: 4604
module Conf;

public class Widget
  public Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;

  destructor
    Console.WriteLine($"disposed {Name}");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var m := new OwningSlotMap<Widget>();
    _ := m.Insert(new Widget("a"));
    _ := m.Insert(new Widget("b"));
    _ := m.Insert(new Widget("c"));
    m.Clear();
    Console.WriteLine($"cleared {m.Count}");
    _ := m.Insert(new Widget("d"));
  end Main;
end Program;
