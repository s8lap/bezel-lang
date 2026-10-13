//! case: s11_2_0103
//! doc: spec
//! section: 11.2
//! title: a function-typed parameter substituted through a class type parameter matches
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   The interface's substitution maps S to the implementing class's argument Z, so (S) -> int under I<int> is (int) -> int, which the implementation's (Z) -> int under K<int> matches.
//! cites: 11.2
//! expect: run
//! stdout: 3
//! issue: 6088
module Conf;

public interface I<S>
  method P(f: (S) -> int): int;
end I;

public class K<Z>: I<Z>
  public constructor() end constructor;

  public method P(f: (Z) -> int): int
    return 3;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I<int> := new K<int>();
    Console.WriteLine(s.P(lambda(x: int) => x));
    return 0;
  end Main;
end C;
