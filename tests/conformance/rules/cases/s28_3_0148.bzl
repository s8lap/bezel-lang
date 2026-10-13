//! case: s28_3_0148
//! doc: spec
//! section: 28.3
//! title: a captured counter and a captured binding assigned a parameter stay usable in scope
//! rule: a captured value that carries no borrow — a value type that holds none, a
//!   null-source value, a `string` — contributes nothing, and lives in its heap
//!   capture cell ... A captured binding — a local or parameter, in any state,
//!   that a lambda in the body captures — holds ... every value it is ever
//!   assigned
//! cites: 28.3, 27.5
//! expect: run
//! stdout: 7
//! issue: 5718
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Program
  public static method Total(xs: Array<int>): int
    var n := 0;
    var add := lambda(v: int) => do
      n := n + v;
    end;
    for var x in xs do
      add(x);
    end;
    return n;
  end Total;

  public static method Probe(p: Item): int
    var b: Item? := null;
    var f := lambda() => b!.N();
    b := p;
    return f();
  end Probe;

  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    using var it := new Item();
    Console.WriteLine((Total(xs) + Probe(it)).ToString());
  end Main;
end Program;
