//! case: s11_2_0078
//! doc: spec
//! section: 11.2
//! title: an Array of a generic parameter type matches a renamed own parameter by position
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<T>(a: Array<Box<T>>) is matched by P<X>(a: Array<Box<X>>): the element type Box<T> under the position correspondence is Box<X>.
//! cites: 11.2
//! expect: run
//! stdout: 7
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public interface I
  method P<T>(a: Array<Box<T>>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<X>(a: Array<Box<X>>): int
    return 7;
  end P;
end K;

public class C
  public static method Main(): int
    var s: I := new K();
    var xs: Array<Box<int>> := [];
    Console.WriteLine(s.P<int>(xs));
    return 0;
  end Main;
end C;
