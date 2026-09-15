//! case: s27_5_0006
//! doc: spec
//! section: 27.5
//! title: a closure may not be stored in a field while it captures a borrow
//! rule: A closure is a borrow-carrying value whose source set is the union of
//!   its captures' sources; while it captures anything, it may not be stored
//!   in a field or a collection (§28.3, v0.298).
//! cites: 27.5, 28.3
//! expect: reject
//! diagnostic: stored in a field
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Greet()
    Console.WriteLine("hi");
  end Greet;
end Widget;

public class Holder
  private _cb: () -> ();

  public method Capture(w: Widget)
    _cb := lambda() => w.Greet();
  end Capture;
end Holder;
