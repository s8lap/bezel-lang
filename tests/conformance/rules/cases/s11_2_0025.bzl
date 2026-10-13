//! case: s11_2_0025
//! doc: spec
//! section: 11.2
//! title: an own type parameter in a function-typed argument does not match a concrete function type
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<T>(a: Array<(T) -> T>) is not matched by P<X>(a: Array<(int) -> int>): the interface's function type is generic in its own parameter and the implementation's is concrete.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6053
module Conf;

public interface I
  method P<T>(a: Array<(T) -> T>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<X>(a: Array<(int) -> int>): int
    return 1;
  end P;
end K;
