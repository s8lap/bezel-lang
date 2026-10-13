//! case: s11_2_0017
//! doc: spec
//! section: 11.2
//! title: a swapped own type parameter nested inside a generic parameter type does not match
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<T, U>(a: Box<Box<T>>) is not matched by P<A, B>(a: Box<Box<B>>).
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
  method P<T, U>(a: Box<Box<T>>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: Box<Box<B>>): int
    return 1;
  end P;
end K;
