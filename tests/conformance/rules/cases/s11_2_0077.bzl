//! case: s11_2_0077
//! doc: spec
//! section: 11.2
//! title: a Pair parameter with renamed own parameters matches by position
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<T, U>(a: Pair<T, U>) is matched by P<A, B>(a: Pair<A, B>): each own parameter corresponds by position.
//! cites: 11.2
//! expect: run
//! stdout: 5
//! issue: 6062
module Conf;

public class Pair<L, R>
  public constructor() end constructor;
end Pair;

public interface I
  method P<T, U>(a: Pair<T, U>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: Pair<A, B>): int
    return 5;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    Console.WriteLine(s.P<int, string>(new Pair<int, string>()));
    return 0;
  end Main;
end C;
