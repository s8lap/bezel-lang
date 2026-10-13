//! case: s11_2_0014
//! doc: spec
//! section: 11.2
//! title: a renamed own type parameter inside a function-typed argument still matches
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by position, not by name. Interface P<T>(a: Array<(T) -> T>) is matched by P<X>(a: Array<(X) -> X>), and the call through the interface reaches it.
//! cites: 11.2
//! expect: run
//! stdout: 0
//! issue: 6053
module Conf;

public interface I
  method P<T>(a: Array<(T) -> T>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<X>(a: Array<(X) -> X>): int
    return a.Count;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    var fs: Array<(int) -> int> := [];
    Console.WriteLine(s.P<int>(fs));
    return 0;
  end Main;
end C;
