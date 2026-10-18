//! case: s28_3_0203
//! doc: spec
//! section: 28.3
//! title: a receiver-sourced drain frame returned from a method reaches the caller and yields its elements
//! rule: A non-iterator method may therefore compose and return such a frame —
//!   provided every borrow the returned frame carries is sourced from the
//!   method's own parameters or receiver (directly or through the receiver's
//!   fields, the receiver-sourced notion of §28.6).
//! cites: 28.3, 28.6, 29.2, 29.1
//! expect: run
//! output: 3
//!   0
//! issue: 6452
module Conf;

public class Item: IDisposable
  public static live: int := 0;

  public constructor()
    Item.live := Item.live + 1;
  end constructor;

  destructor Item.live := Item.live - 1; end destructor;

  public method Tag(): string
    return "i";
  end Tag;
end Item;

// The callee side the rule is about: a plain method whose declared return type
// is one of the mint-announcing kinds (the announcement table's row 2), whose
// body returns a frame built by a deferred-activation call over its OWN
// receiver's field. The receiver-sourced carve-out is what makes this admitted
// rather than the reject its scope-bound siblings get.
public class Bag
  public owned items: OwningArray<Item>;

  public constructor()
    items := new OwningArray<Item>();
    items.Add(new Item());
    items.Add(new Item());
    items.Add(new Item());
  end constructor;

  public method View(): IOwningEnumerable<Item>
    return items.Drain();
  end View;
end Bag;

public class Program
  public static method Main()
    using var bag := new Bag();
    var n := 0;
    using var view: IOwningEnumerable<Item> := bag.View();
    for var i in view do
      n := n + 1;
    end for;
    // Two lines, so BOTH halves of the rule are observable. `n` is 3 — every
    // element the frame carried actually reached the caller, which is what
    // fails if the returned frame is null or empty. `Item.live` is 0 — each
    // element was discharged exactly once (§29.2's owning desugaring hands
    // each to the loop variable, §28.3's affine auto-drop releases it at the
    // loop's end), which is what fails if the frame double-disposes or leaks.
    Console.WriteLine($"{n}");
    Console.WriteLine($"{Item.live}");
  end Main;
end Program;