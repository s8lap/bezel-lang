//! case: s11_2_0074
//! doc: spec
//! section: 11.2
//! title: a Dictionary parameter with a generic value type matches a renamed own parameter pair by position
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<K2, V2>(a: Dictionary<K2, Box<V2>>) is matched by P<A, B>(a: Dictionary<A, Box<B>>): each own parameter corresponds by position.
//! cites: 11.2
//! expect: run
//! stdout: 8
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I
  method P<K2, V2>(a: Dictionary<K2, Box<V2>>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: Dictionary<A, Box<B>>): int
    return 8;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    var d := new Dictionary<int, Box<string>>();
    Console.WriteLine(s.P<int, string>(d));
    return 0;
  end Main;
end C;
