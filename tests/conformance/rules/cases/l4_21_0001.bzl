//! case: l4_21_0001
//! doc: stdlib
//! section: 4.21
//! title: Insert mints a handle, GetOrNull reads the stored value as a borrow, and the map's disposal cascades
//! rule: Insert(owned value: T): SlotHandle takes ownership and mints a
//!   handle; GetOrNull(handle) returns a borrow of the stored value when the
//!   handle is live; disposing the map disposes every value it still holds.
//! cites: 4.21, 28.11
//! expect: run
//! output: a
//!   disposed a
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
    var w := m.GetOrNull(h);
    if w <> null then
      Console.WriteLine(w.Name);
    end if;
  end Main;
end Program;
