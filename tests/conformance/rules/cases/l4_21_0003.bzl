//! case: l4_21_0003
//! doc: stdlib
//! section: 4.21
//! title: a stale handle after slot reuse reads absent while the new handle for the same slot reads live
//! rule: Insert reuses the most recently vacated slot; a handle is live iff
//!   its slot holds a value and the slot's generation equals the handle's
//!   Generation, so a handle minted before the slot was vacated never reads
//!   a later occupant of its slot.
//! cites: 4.21
//! expect: run
//! output: disposed a
//!   False True False True
//!   disposed b
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
    var h1 := m.Insert(new Widget("a"));
    _ := m.Remove(h1);
    var h2 := m.Insert(new Widget("b"));
    Console.WriteLine($"{m.Contains(h1)} {m.Contains(h2)} {h1 = h2} {h1.Index = h2.Index}");
  end Main;
end Program;
