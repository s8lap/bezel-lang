//! case: s11_2_0022
//! doc: spec
//! section: 11.2
//! title: an own type parameter does not match a concrete type argument
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<T>(a: Box<T>) is not matched by P<X>(a: Box<int>): the interface's argument is its own parameter, and the implementation's is the concrete int.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6062
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;

public class Pair<L, R>
  public constructor() end constructor;
end Pair;

public interface I
  method P<T>(a: Box<T>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<X>(a: Box<int>): int
    return 1;
  end P;
end K;
