//! case: s27_3_9706
//! doc: spec
//! section: 27.3
//! title: an int-returning lambda converts at a () -> long indexer slot
//! rule: `var f: (int) -> long := lambda(x) => x * 2;` is admitted, the `int` body widening to `long`
//! cites: 27.3, 5.1, 23.4
//! expect: run
//! output: 8
//! notes: Pins #9230 (pick 16a3dc690): an index position that supplies a function-typed target is judged
//!   like a call argument: the body is typed and its inferred return type must equal the slot's
//!   return type or implicitly convert to it, otherwise the lambda is a compile error at the position
//!   (the method twin was already refused; the indexer path admitted the lambda and the accessor
//!   called it through the wrong ABI). The implicit-conversion half of the admission rule: `7` is an
//!   int body at a long return slot, so it is admitted; the accessor adds 1L: 8. A fix that demanded
//!   exact equality would fail here.
module Conf;

public class C
  public property [f: () -> long]: long => f() + 1L;
  public constructor()
  end constructor;
end C;

public class Program
  public static method Main(): int
    using var c := new C();
    var n := c[lambda() => 7];
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
