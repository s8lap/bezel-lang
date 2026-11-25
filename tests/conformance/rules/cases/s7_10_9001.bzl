//! case: s7_10_9001
//! doc: spec
//! section: 7.10
//! title: a closure returned as () -> T? over an unconstrained T yields the captured int, or null-coalesces
//! rule: `T?` always means "the nullable form of T," regardless of constraint.
//! cites: 14.1, 7.2, 27.5
//! expect: run
//! output: 19900
//!   7
//!   9
//! notes: Harvested from the #9120 review (a1). `M<T>(p: T): () -> T?` returns `lambda(): T? => p`; at T = int
//!   the result type is `int?`, and a present value reads back through `r!` and `??`. Trace: sum of i
//!   over 0..199 = 19900; M<int>(7)() ?? -1 = 7; M<int>(9)() ?? -1 = 9. Value-only; no leak leg in the
//!   corpus.
module Conf;
public class Program
  public static method M<T>(p: T): () -> T?
    return lambda(): T? => p;
  end M;
  public static method Main(): int
    var tot := 0;
    var i := 0;
    while i < 200 do
      var f := Program.M<int>(i);
      var r := f();
      if r <> null then tot := tot + r!; end if;
      i := i + 1;
    end;
    Console.WriteLine(tot);
    var g := Program.M<int>(7);
    Console.WriteLine(g() ?? -1);
    Console.WriteLine(Program.M<int>(9)() ?? -1);
    return 0;
  end Main;
end Program;
