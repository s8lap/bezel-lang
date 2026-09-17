//! case: l4_5_0012
//! doc: stdlib
//! section: 4.5
//! title: OwningArray<T> declares no Sorted() — no owning copy is expressible
//! rule: OwningArray<T> declares no copy-forms of its own — no Reversed,
//!   no Sorted, no ToArray — because no owning copy is expressible: it
//!   would either double-own each element or drain the source.
//! cites: 4.5
//! expect: reject
//! diagnostic: Sorted
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    using var pool := new OwningArray<Widget>();
    using var copy := pool.Sorted(lambda(a: Widget, b: Widget): int => 0);
  end Demo;
end Program;
