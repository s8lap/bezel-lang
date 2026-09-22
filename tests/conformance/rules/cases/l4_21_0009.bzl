//! case: l4_21_0009
//! doc: stdlib
//! section: 4.21
//! title: a moved owned binding at Insert's owned position compiles warning-free
//! rule: Insert(owned value: T) takes ownership: a fresh producer or a moved
//!   binding is admitted, and the moved binding needs no other discharge.
//! cites: 4.21, 28.3
//! expect: quiet
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
  public method Fill()
    using var m := new OwningSlotMap<Widget>();
    var w := new Widget("a");
    var h := m.Insert(move w);
    if m.Contains(h) then
      Console.WriteLine("live");
    end if;
  end Fill;
end Program;
