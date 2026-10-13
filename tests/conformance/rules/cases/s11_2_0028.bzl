//! case: s11_2_0028
//! doc: spec
//! section: 11.2
//! title: a class type parameter substitutes into a function-typed argument of a generic member
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Under the interface's substitution the class parameter C becomes int, so I<int>'s P<T>(a: Array<(T) -> C>) is matched by P<X>(a: Array<(X) -> int>), and the call through I<int> reaches it.
//! cites: 11.2
//! expect: run
//! stdout: 0
//! issue: 6053
module Conf;

public interface I<C>
  method P<T>(a: Array<(T) -> C>): int;
end I;

public class K: I<int>
  public constructor() end constructor;

  public method P<X>(a: Array<(X) -> int>): int
    return a.Count;
  end P;
end K;

public class C0
  public static method Main(): int
    var s: I<int> := new K();
    var fs: Array<(string) -> int> := [];
    Console.WriteLine(s.P<string>(fs));
    return 0;
  end Main;
end C0;
