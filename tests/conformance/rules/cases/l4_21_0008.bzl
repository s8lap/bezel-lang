//! case: l4_21_0008
//! doc: stdlib
//! section: 4.21
//! title: an unmarked read of an owned binding at Insert's owned position is rejected
//! rule: Insert(owned value: T) takes ownership: a fresh producer or a moved
//!   binding, never an unmarked read of an owned binding (spec §28.3 K3.3 —
//!   an owned argument is spelled `move x` at the call site).
//! cites: 4.21, 28.3
//! expect: reject
//! diagnostic: move
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
  public method Fill()
    using var m := new OwningSlotMap<Widget>();
    var w := new Widget("a");
    _ := m.Insert(w);
  end Fill;
end Program;
