//! case: s11_2_0082
//! doc: spec
//! section: 11.2
//! title: a concrete function return in a parameter does not match an own parameter in the same slot
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P<T>(a: Array<(T) -> int>) has the return of its function type fixed to int; the implementation replaces that return with its own parameter X, which is not the same type.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6053
module Conf;

public interface I
  method P<T>(a: Array<(T) -> int>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<X>(a: Array<(X) -> X>): int
    return 1;
  end P;
end K;
