//! case: s28_4_0004
//! doc: spec
//! section: 28.4
//! title: an owning-inline-array field disposes only its non-null slots
//! rule: Disposing all owned-bearing fields covers fields of owning-inline-array type
//!   (`[N owned]T?`): each non-null slot's `Dispose()` is called, and a null slot
//!   disposes nothing.
//! cites: 15.1.1
//! expect: run
//! output: dispose a
module Conf;

public class Widget
  private _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {_name}");
  end destructor;
end Widget;

public class Basket
  private _slots: [2 owned]Widget?;

  public constructor()
    _slots := [new Widget("a"), null];
  end constructor;
end Basket;

public class Program
  public static method Main()
    using var b := new Basket();
  end Main;
end Program;
