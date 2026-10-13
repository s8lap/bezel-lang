//! case: s11_2_0073
//! doc: spec
//! section: 11.2
//! title: an interface generic argument substituted with the matching concrete type matches
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. The interface I<int> substitutes S by int, so its member is P(a: Box<int>), and the implementation's P(a: Box<int>) matches it; the call through the interface reaches it.
//! cites: 11.2
//! expect: run
//! stdout: 11
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I<S>
  method P(a: Box<S>): int;
end I;

public class K: I<int>
  public constructor() end constructor;

  public method P(a: Box<int>): int
    return 11;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I<int> := new K();
    Console.WriteLine(s.P(new Box<int>()));
    return 0;
  end Main;
end C;
