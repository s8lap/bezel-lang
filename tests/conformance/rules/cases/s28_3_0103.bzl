//! case: s28_3_0103
//! doc: spec
//! section: 28.3
//! title: a concretely typed argument to the invoked source parameter is admitted at a class B
//! rule: every return path must supply `null` or a borrow whose sources outlive
//!   the call: ... a plain source parameter — directly, a borrow reached
//!   through it, or the result of invoking a function-typed source parameter,
//!   itself classified by this rule, so that each plain argument of the
//!   invocation is a source of the result and must be admissible here in turn
//! cites: 28.3
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Fns
  public static method MapI<B>(a: int, f: (int) -> B): B => f(a);
end Fns;

public class Client
  public static method Pick(p: Item): int
    var r := Fns.MapI<Item>(1, lambda(v) => p);
    return r.Size();
  end Pick;
end Client;
