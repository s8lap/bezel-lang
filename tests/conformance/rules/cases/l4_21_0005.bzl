//! case: l4_21_0005
//! doc: stdlib
//! section: 4.21
//! title: disposing the map disposes its values in descending Index
//! rule: The map's own Dispose() disposes the values in descending Index —
//!   the ordered families' reverse-storage-order rule of spec §28.11, storage
//!   order here being slot order — then releases the storage.
//! cites: 4.21, 28.11
//! expect: run
//! output: disposed c
//!   disposed b
//!   disposed a
//!   after
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
    do
      using var m := new OwningSlotMap<Widget>();
      _ := m.Insert(new Widget("a"));
      _ := m.Insert(new Widget("b"));
      _ := m.Insert(new Widget("c"));
    end;
    Console.WriteLine("after");
  end Main;
end Program;
