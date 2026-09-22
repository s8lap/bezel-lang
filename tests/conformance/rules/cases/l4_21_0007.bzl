//! case: l4_21_0007
//! doc: stdlib
//! section: 4.21
//! title: a handle minted by another map raises ArgumentException
//! rule: Every handle-taking member raises ArgumentException when the handle
//!   was minted by a different OwningSlotMap<T> instance — a caller bug, not
//!   an expected absence.
//! cites: 4.21, 1
//! expect: run
//! output: foreign
//!   disposed b
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
    using var m1 := new OwningSlotMap<Widget>();
    using var m2 := new OwningSlotMap<Widget>();
    var h1 := m1.Insert(new Widget("a"));
    _ := m2.Insert(new Widget("b"));
    try
      var probe := m2.Contains(h1);
      Console.WriteLine($"probe {probe}");
    catch
      ArgumentException => Console.WriteLine("foreign")
    end try;
  end Main;
end Program;
