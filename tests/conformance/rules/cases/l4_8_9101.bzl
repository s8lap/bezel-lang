//! case: l4_8_9101
//! doc: stdlib
//! section: 4.8
//! title: a plain `(int) -> K` local is admitted at the borrowing Dictionary's `factory` slot
//! rule: public method GetOrAdd(key: K, factory: (K) -> V): V;
//! cites: 4.8, 27.1.1
//! expect: run
//! output: 7
//! notes: Pins #9146 (pick e35d147ea): the stdlib declares an `owned` function slot on the
//!   built-in; a plain `(..) -> K` function value is a modifier mismatch exactly as at a
//!   user-declared `-> owned K` slot (cf. s27_1_1_8834). Spec-decided control: the borrowing
//!   Dictionary's factory slot has no `owned` modifier, so a plain function value matches it
//!   exactly. Green on the base too.
module Conf;

public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Main(): int
    using var k := new K(7);
    var f: (int) -> K := lambda(i: int) => k;
    using var d := new Dictionary<int, K>();
    Console.WriteLine(d.GetOrAdd(1, f).N);

    return 0;
  end Main;
end Program;
