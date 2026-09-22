//! case: l4_21_0002
//! doc: stdlib
//! section: 4.21
//! title: Remove disposes the value and the handle reads absent afterwards
//! rule: Remove(handle) disposes the value and returns true; a handle whose
//!   slot has been vacated reads as absent — Contains is false and GetOrNull
//!   is null — for the rest of the map's life.
//! cites: 4.21, 28.11
//! expect: run
//! output: disposed a
//!   True True False 0
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
    var h := m.Insert(new Widget("a"));
    var removed := m.Remove(h);
    var again := m.GetOrNull(h);
    Console.WriteLine($"{removed} {again = null} {m.Contains(h)} {m.Count}");
  end Main;
end Program;
