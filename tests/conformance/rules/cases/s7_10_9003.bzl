//! case: s7_10_9003
//! doc: spec
//! section: 7.10
//! title: a closure returned as () -> T? over an unconstrained T is usable at long, bool, double and byte
//! rule: `T?` always means "the nullable form of T," regardless of constraint.
//! cites: 14.1, 7.2, 27.5
//! expect: run
//! output: 5000000000
//!   True
//!   2.5
//!   200
//! notes: Harvested from the #9120 review (a3). Each instantiation of `M<T>` yields `T?` = the nullable form
//!   of the primitive. Rendering: long and byte print their digits, bool prints True (spec 8.5), double
//!   2.5 is the shortest round-trip string (stdlib 2.18).
module Conf;
public class Program
  public static method M<T>(p: T): () -> T?
    return lambda(): T? => p;
  end M;
  public static method Main(): int
    var f := Program.M<long>(5000000000L);
    Console.WriteLine(f() ?? -1L);
    var g := Program.M<bool>(true);
    Console.WriteLine(g() ?? false);
    var h := Program.M<double>(2.5);
    Console.WriteLine(h() ?? -1.0);
    var b: byte := 200;
    var k := Program.M<byte>(b);
    var kb: byte := 0;
    var q := k() ?? kb;
    Console.WriteLine(q);
    return 0;
  end Main;
end Program;
