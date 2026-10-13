//! case: s11_2_0008
//! doc: spec
//! section: 11.2
//! title: a renamed own type parameter inside a generic parameter type still matches
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters take part by number and position, not by name. Interface P<T>(a: Box<T>) is matched by P<X>(a: Box<X>), and the call through the interface reaches it.
//! cites: 11.2
//! expect: run
//! stdout: 1
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I
  method P<T>(a: Box<T>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<X>(a: Box<X>): int
    return 1;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    Console.WriteLine(s.P<int>(new Box<int>()));
    return 0;
  end Main;
end C;
