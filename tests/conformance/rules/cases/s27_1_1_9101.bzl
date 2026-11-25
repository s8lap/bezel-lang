//! case: s27_1_1_9101
//! doc: spec
//! section: 27.1.1
//! title: a plain `(int) -> K` local is rejected at OwningBuffer's `init: (int) -> owned T` slot
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 4.14
//! expect: reject
//! diagnostic: modifier slots
//! issue: 9146
//! notes: Pins #9146 (pick e35d147ea): the stdlib declares an `owned` function slot on the
//!   built-in; a plain `(..) -> K` function value is a modifier mismatch exactly as at a
//!   user-declared `-> owned K` slot (cf. s27_1_1_8834). stdlib 4.14: `public
//!   constructor(length: int, init: (int) -> owned T);`. Accepting twin: l4_14_9101 (the same
//!   constructor with an `owned`-typed local).
module Conf;

public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Main(): int
    using var k := new K(7);
    var f: (int) -> K := lambda(i: int) => k;
    using var b := new OwningBuffer<K>(2, f);

    return 0;
  end Main;
end Program;
