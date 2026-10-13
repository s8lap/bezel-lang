//! case: s11_2_0083
//! doc: spec
//! section: 11.2
//! title: a function-typed return with identically named own parameters matches
//! rule: Return types must match exactly under the interface's substitution, and own type parameters correspond by number and position, not by name. Interface P<T, U>(a: T): Array<(T) -> U> is matched by an implementation with the same names; the call through the interface reaches it.
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

  public method P<T, U>(a: T): Array<(T) -> U>
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
