//! case: l4_21_0010
//! doc: stdlib
//! section: 4.21
//! title: Dispose through the borrow GetOrNull returns is rejected
//! rule: Get, GetOrNull and TryGetValue produce borrows sourced from the map;
//!   disposing through such a borrow is spec §28.6's compile error — the map
//!   owns the value, and Remove/PopValue are the forms that release it.
//! cites: 4.21, 28.6
//! expect: reject
//! diagnostic: borrow
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
  public method Drop()
    using var m := new OwningSlotMap<Widget>();
    var h := m.Insert(new Widget("a"));
    var w := m.GetOrNull(h);
    w!.Dispose();
  end Drop;
end Program;
