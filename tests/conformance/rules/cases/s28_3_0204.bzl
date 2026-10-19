//! case: s28_3_0204
//! doc: spec
//! section: 28.3
//! title: the owned-marked, generic receiver-sourced drain forwarder reaches the caller with its elements
//! rule: A non-iterator method may therefore compose and return such a frame
//!   ... (an instance or extension callee states the same contract in the same
//!   unmarked spelling; `owned` remains admissible emphasis) — provided every
//!   borrow the returned frame carries is sourced from the method's own
//!   parameters or receiver (directly or through the receiver's fields, the
//!   receiver-sourced notion of §28.6).
//! cites: 28.3, 28.6, 29.2, 29.1, 14.4
//! expect: run
//! output: 3
//!   0
//! issue: 6464
module Conf;

public class Res: IDisposable
  public static live: int := 0;

  public constructor()
    Res.live := Res.live + 1;
  end constructor;

  destructor Res.live := Res.live - 1; end destructor;
end Res;

// §14.4's own composition shape, at the spelling its `owned`-marked return
// names: a GENERIC receiver, a PRIVATE owned field, `owned IOwningEnumerable<T>`
// declared. s28_3_0203 covers the unmarked spelling over a non-generic class
// with a public field; this row is what keeps the `owned` emphasis — and the
// receiver's own type parameter — inside what the rule reaches.
//
// `_items` is initialized with the explicit `new OwningArray<T>()`, not the
// `new()` shorthand the doc example writes: the shorthand at a typed ASSIGNMENT
// target is not admitted (§17.7), the compiler accepts it and drops the store
// anyway, and leaving it in would make this case fail for a reason other than
// the rule it pins. Filed as #6482.
public class OwningCache<T> where T: class
  private owned _items: OwningArray<T>;

  public constructor()
    _items := new OwningArray<T>();
  end constructor;

  public method Add(owned item: T)
    _items.Add(move item);
  end Add;

  public method Drain(): owned IOwningEnumerable<T>
    return _items.Drain();
  end Drain;
end OwningCache;

public class Program
  public static method Main()
    using var c := new OwningCache<Res>();
    c.Add(new Res());
    c.Add(new Res());
    c.Add(new Res());

    var n := 0;
    using var view: IOwningEnumerable<Res> := c.Drain();
    for var r in view do
      n := n + 1;
    end for;

    // Two lines, so both halves of the rule are observable. `n` is 3 — every
    // element the frame carried reached the caller, which is what fails if the
    // returned frame is null or empty. `Res.live` is 0 — each element was
    // discharged exactly once (§29.2's owning desugaring hands each to the
    // loop variable, §28.3's affine auto-drop releases it at the loop's end),
    // which is what fails if the frame double-disposes or leaks.
    Console.WriteLine($"{n}");
    Console.WriteLine($"{Res.live}");
  end Main;
end Program;