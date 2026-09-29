//! case: l4_21_0006
//! doc: stdlib
//! section: 4.21
//! title: Drain yields owned values in ascending Index and vacates each slot as it goes
//! rule: Drain() returns an IOwningEnumerable<T> whose iteration transfers
//!   each value to the consumer in ascending Index and vacates its slot; after
//!   full iteration the map is empty.
//! cites: 4.21, 4.13, 28.11
//! expect: run
//! output: drained a
//!   disposed a
//!   drained b
//!   disposed b
//!   left 0
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
    for var v in m.Drain() do
      using var w := move v;
      Console.WriteLine($"drained {w.Name}");
    end for;
    Console.WriteLine($"left {m.Count}");
  end Main;
end Program;
