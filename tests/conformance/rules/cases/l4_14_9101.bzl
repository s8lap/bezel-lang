//! case: l4_14_9101
//! doc: stdlib
//! section: 4.14
//! title: an `(int) -> owned K` local is admitted at OwningBuffer's `init` slot and builds the
//!   buffer
//! rule: public constructor(length: int, init: (int) -> owned T);
//! cites: 4.14, 27.1.1
//! expect: run
//! output: 2
//! notes: Pins #9146 (pick e35d147ea): the stdlib declares an `owned` function slot on the
//!   built-in; a plain `(..) -> K` function value is a modifier mismatch exactly as at a
//!   user-declared `-> owned K` slot (cf. s27_1_1_8834). Spec-decided admitting twin: the
//!   modifier slots match, so the value is admitted. Green on the base too.
module Conf;

public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Main(): int
    var f: (int) -> owned K := lambda(i: int) => new K(i + 1);
    using var b := new OwningBuffer<K>(2, f);
    Console.WriteLine(b[1]!.N);

    return 0;
  end Main;
end Program;
