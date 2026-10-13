//! case: s11_2_0099
//! doc: spec
//! section: 11.2
//! title: a class type parameter substituted into an inline-array parameter matches
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   The interface's substitution maps S to the implementing class's argument Z, so [3]S under I<int> is [3]int, which the implementation's [3]Z under K<int> matches.
//! cites: 11.2
//! expect: run
//! stdout: 5
//! issue: 6088
module Conf;

public interface I<S>
  method P(a: [3]S): int;
end I;

public class K<Z>: I<Z>
  public constructor() end constructor;

  public method P(a: [3]Z): int
    return 5;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I<int> := new K<int>();
    var arr: [3]int;
    Console.WriteLine(s.P(arr));
    return 0;
  end Main;
end C;
