//! case: s11_2_0020
//! doc: spec
//! section: 11.2
//! title: a nullable generic parameter matches a renamed own type parameter and accepts null
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<T>(a: Box<T>?) is matched by P<X>(a: Box<X>?), and the call through the interface reaches it.
//! cites: 11.2
//! expect: run
//! stdout: 6
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public class Pair<L, R>
  public constructor() end constructor;
end Pair;

public interface I
  method P<T>(a: Box<T>?): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<X>(a: Box<X>?): int
    return 6;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    Console.WriteLine(s.P<int>(null));
    return 0;
  end Main;
end C;
