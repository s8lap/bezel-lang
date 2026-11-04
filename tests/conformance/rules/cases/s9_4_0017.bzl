//! case: s9_4_0017
//! doc: spec
//! section: 9.4
//! title: a record member runs over the value its receiver expression denoted at the call
//! rule: A member call on a **copyable** value-kind receiver — … — runs over the
//!   value its receiver expression denoted when that expression was evaluated
//!   … no store that runs during the call — to the storage the receiver
//!   expression named, through any alias of it, or to anything else — changes
//!   what `this` denotes, and the activation's completion stores nothing back.
//! expect: run
//! output: 1
//!   99
//! notes: the spec's own Snap/Shelf example.
module Conf;

public record Snap(N: int)
  public method Show(shelf: Shelf)
    shelf.Item := new Snap(99);
    Console.WriteLine($"{N}");
  end Show;
end Snap;

public class Shelf
  public Item: Snap := new Snap(1);
end Shelf;

public class Program
  public static method Main()
    using var s := new Shelf();
    s.Item.Show(s);
    Console.WriteLine($"{s.Item.N}");
  end Main;
end Program;
