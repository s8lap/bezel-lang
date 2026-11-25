//! case: s27_1_1_8714
//! doc: spec
//! section: 27.1.1
//! title: an owned-return value picks the overload whose return slot is owned
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 22.6
//! expect: run
//! stdout: 2
//! notes: Modifier slots are part of a function type's identity, so `Pick(() -> K)` and `Pick(() ->
//!   owned K)` are distinct overloads, and the `() -> owned K` argument is applicable only to
//!   the second: it prints 2.
module Conf;
public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Pick(f: () -> K): int => 1;
  public static method Pick(f: () -> owned K): int => 2;
  public static method Main(): int
    var f: () -> owned K := lambda() => new K(5);
    Console.WriteLine($"{Program.Pick(f)}");
    return 0;
  end Main;
end Program;
