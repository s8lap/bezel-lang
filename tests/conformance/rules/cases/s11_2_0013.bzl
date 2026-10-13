//! case: s11_2_0013
//! doc: spec
//! section: 11.2
//! title: a function-typed argument with its own type parameters swapped by position does not match
//! rule: A member's own type parameters correspond by number and position, not by name. Interface P<T, U>(a: Array<(T) -> U>) is not matched by P<A, B>(a: Array<(B) -> A>): the parameter and return of the function type trade positions.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6053
module Conf;

public interface I
  method P<T, U>(a: Array<(T) -> U>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: Array<(B) -> A>): int
    return 1;
  end P;
end K;
