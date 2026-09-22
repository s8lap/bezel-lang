//! case: l4_21_0011
//! doc: stdlib
//! section: 4.21
//! title: default(SlotHandle) denotes a slot of no map and raises ArgumentException
//! rule: A default(SlotHandle) (spec §17.11) denotes a slot of no map and
//!   takes the same fault as a handle minted by a different OwningSlotMap<T>
//!   instance: every handle-taking member raises ArgumentException — no map
//!   ever mints it.
//! cites: 4.21, 1
//! expect: run
//! output: foreign
//!   disposed a
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
    var none := default(SlotHandle);
    try
      var probe := m.Contains(none);
      Console.WriteLine($"probe {probe}");
    catch
      ArgumentException => Console.WriteLine("foreign")
    end try;
  end Main;
end Program;
