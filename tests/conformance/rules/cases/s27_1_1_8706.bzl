//! case: s27_1_1_8706
//! doc: spec
//! section: 27.1.1
//! title: an owned-return function value at a plain return slot of a generic call is rejected
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5
//! expect: reject
//! diagnostic: modifier slots
//! notes: Modifier slots must match exactly. `Apply<K>` instantiates the parameter `() -> T` at the
//!   plain class slot `() -> K`, and the argument `f` is `() -> owned K`: the `owned` return
//!   slot differs from the plain one, so the call is rejected at the argument. Twin:
//!   s27_1_1_8707, where the target slot is `owned`.
module Conf;
public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Apply<T>(f: () -> T): T
    return f();
  end Apply;
  public static method Main(): int
    var f: () -> owned K := lambda() => new K(5);
    var k := Program.Apply<K>(f);
    return k.N;
  end Main;
end Program;
