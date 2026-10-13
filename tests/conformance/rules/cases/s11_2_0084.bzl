//! case: s11_2_0084
//! doc: spec
//! section: 11.2
//! title: a function-typed return with a concrete return type and a renamed own parameter matches
//! rule: Return types must match exactly under the interface's substitution, and own type parameters correspond by number and position, not by name. Interface P<T>(a: T): Array<(T) -> int> has a concrete return int in the function type; the renamed implementation P<A>(a: A): Array<(A) -> int> carries the same concrete return and matches.
//! cites: 11.2
//! expect: run
//! stdout: 0
//! issue: 6076
module Conf;

public interface I
  method P<T>(a: T): Array<(T) -> int>;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A>(a: A): Array<(A) -> int>
    return [];
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    var b := s.P<int>(1);
    Console.WriteLine(b.Count);
    return 0;
  end Main;
end C;
