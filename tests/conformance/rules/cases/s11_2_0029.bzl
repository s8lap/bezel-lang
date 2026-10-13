//! case: s11_2_0029
//! doc: spec
//! section: 11.2
//! title: a renamed function-typed return with a renamed own type parameter matches by position
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Return types must match exactly under the interface's substitution. Interface P<T, U>(a: T): Array<(T) -> U> is matched by P<A, B>(a: A): Array<(A) -> B>: each own parameter corresponds by position, so the renamed names still match, and the call through the interface reaches it.
//! cites: 11.2
//! expect: run
//! stdout: 0
//! issue: 6076
module Conf;

public interface I
  method P<T, U>(a: T): Array<(T) -> U>;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: A): Array<(A) -> B>
    return [];
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    var b := s.P<int, string>(1);
    Console.WriteLine(b.Count);
    return 0;
  end Main;
end C;
