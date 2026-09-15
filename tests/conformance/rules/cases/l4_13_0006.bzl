//! case: l4_13_0006
//! doc: stdlib
//! section: 4.13
//! title: a fresh GetOwningEnumerator() on a live drain view continues over the current remainder
//! rule: Each GetOwningEnumerator() call on a live view yields a fresh
//!   enumerator over the collection's current remainder -- a re-enumeration
//!   continues draining what remains, never replaying what already left.
//! cites: 4.13
//! expect: run
//! stdout: b
module Conf;

public class Widget
  private readonly _name: string;

  public constructor(name: string)
    _name := name;
  end constructor;

  public method Name(): string => _name;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget("a"));
    pool.Add(new Widget("b"));
    using var view := pool.Drain();
    using var it1 := view.GetOwningEnumerator();
    _ := it1.MoveNext();
    using var w1 := it1.TakeCurrent();
    using var it2 := view.GetOwningEnumerator();
    _ := it2.MoveNext();
    using var w2 := it2.TakeCurrent();
    Console.WriteLine(w2.Name());
  end Main;
end Program;
