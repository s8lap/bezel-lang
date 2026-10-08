//! case: s28_3_0101
//! doc: spec
//! section: 28.3
//! title: a bare-T function-value invocation over a type-parameter argument is rejected at Map<int, Item>
//! rule: `static method Map<A, B>(a: A, f: (A) -> B): B => f(a);` is rejected at
//!   every instantiation whose `B` this check reaches, `Map<int, Item>` as
//!   well as `Map<Item, Item>`: `a` is no source parameter, and an argument
//!   typed by a type parameter counts under the weakest assumption its
//!   constraints establish; `Map` is admitted where `B` is a value type that
//!   carries no borrow
//! cites: 28.3
//! expect: reject
//! diagnostic: source
//! issue: 5609
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Fns
  public static method Map<A, B>(a: A, f: (A) -> B): B => f(a);
end Fns;

public class Client
  public static method Pick(p: Item): int
    var r := Fns.Map<int, Item>(1, lambda(v) => p);
    return r.Size();
  end Pick;
end Client;
