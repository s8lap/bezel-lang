//! case: s11_2_0076
//! doc: spec
//! section: 11.2
//! title: a nested generic parameter Box of Box matches a renamed own parameter by position
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<T>(a: Box<Box<T>>) is matched by P<X>(a: Box<Box<X>>): the single own parameter corresponds by position, so the renamed name still matches.
//! cites: 11.2
//! expect: run
//! stdout: 4
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I
  method P<T>(a: Box<Box<T>>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<X>(a: Box<Box<X>>): int
    return 4;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    Console.WriteLine(s.P<int>(new Box<Box<int>>()));
    return 0;
  end Main;
end C;
