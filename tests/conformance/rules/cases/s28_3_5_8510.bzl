//! case: s28_3_5_8510
//! doc: spec
//! section: 28.3.5
//! title: a chained call's borrowed result passed as an argument is not disposed at statement end
//! rule: The result is never owned.
//! cites: 28.3.5, 28.3.11
//! expect: run
//! stdout: 1000 0 200
//! notes: Pins #9040. The chained call's `K` result is a borrow, so the full-expression boundary
//!   of §28.3.11 does not dispose it: its destructor must not have run when the statement
//!   ends. `bad` sums, after each iteration's statement, the destructor count minus the j
//!   iterations already finished (so it is 0 exactly when the borrowed result was not
//!   disposed early). 200 iterations, each reading N = 5: t = 1000, bad = 0, and each K is
//!   destroyed once at its owner's disposal: D = 200. The static counter draws the expected
//!   §22.12 rule 2 declaration-site warning. The chain result is the argument of a further
//!   call, `Take(Use(s)())`.
module Conf;
public class K
  public static D: int := 0;
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
  destructor K.D := K.D + 1; end destructor;
end K;

public class Program
  public static method Use(p: K): () -> K
    var f := lambda() => p;
    return lambda() => f();
  end Use;
  public static method Take(k: K): int => k.N;
  public static method Main(): int
    var t := 0;
    var bad := 0;
    for var j in 0 ..< 200 do
      using var s := new K(5);
      t := t + Program.Take(Program.Use(s)());
      bad := bad + (K.D - j);
    end;
    Console.WriteLine($"{t} {bad} {K.D}");
    return 0;
  end Main;
end Program;
