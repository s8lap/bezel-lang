//! case: s11_2_0080
//! doc: spec
//! section: 11.2
//! title: a non-generic zero-argument function type in an Array parameter matches
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P(a: Array<() -> int>) has no own type parameter; the implementation's parameter is the same type, so it matches and the call through the interface reaches it.
//! cites: 11.2
//! expect: run
//! stdout: 0
//! issue: 6053
module Conf;

public interface I
  method P(a: Array<() -> int>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P(a: Array<() -> int>): int
    return a.Count;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    var fs: Array<() -> int> := [];
    Console.WriteLine(s.P(fs));
    return 0;
  end Main;
end C;
