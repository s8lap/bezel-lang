//! case: s27_1_1_9104
//! doc: spec
//! section: 27.1.1
//! title: a `??` of plain function values is rejected at OwningDictionary.GetOrAdd's `owned`
//!   factory slot
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 4.9
//! expect: reject
//! diagnostic: modifier slots
//! issue: 9146
//! notes: Pins #9146 (pick e35d147ea): the stdlib declares an `owned` function slot on the
//!   built-in; a plain `(..) -> K` function value is a modifier mismatch exactly as at a
//!   user-declared `-> owned K` slot (cf. s27_1_1_8834). The null-coalescing spelling.
module Conf;

public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Main(): int
    using var k := new K(7);
    var f: ((int) -> K)? := null;
    var g: (int) -> K := lambda(i: int) => k;
    using var d := new OwningDictionary<int, K>();
    var v := d.GetOrAdd(1, f ?? g);

    return 0;
  end Main;
end Program;
