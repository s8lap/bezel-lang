//! case: s11_2_0095
//! doc: spec
//! section: 11.2
//! title: an inline-array parameter with a renamed own type parameter matches by position
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   A member's own type parameters take part in the match by number and position, not by name. The interface's substitution maps each of the interface's type parameters to the implementing type's argument, and each of the interface member's own type parameters to the candidate's type parameter at the same position; parameter types are compared after that mapping.
//!   So `[3]U` in the interface member and `[3]W` in the candidate match, with U and W corresponding by position.
//! cites: 11.2
//! expect: run
//! stdout: 6
//! issue: 6088
module Conf;

public interface I
  method P<U>(a: [3]U): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<W>(a: [3]W): int
    return 6;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    var arr: [3]int;
    Console.WriteLine(s.P<int>(arr));
    return 0;
  end Main;
end C;
