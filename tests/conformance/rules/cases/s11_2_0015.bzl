//! case: s11_2_0015
//! doc: spec
//! section: 11.2
//! title: a function-typed return with its own type parameters swapped by position is rejected
//! rule: Return types must match exactly under the interface's substitution, and a member's own type parameters correspond by number and position. Interface P<T, U>(a: T): Array<(T) -> U> is not matched by P<A, B>(a: A): Array<(B) -> A>: the function type's parameter and return trade positions.
//! cites: 11.2
//! expect: reject
//! diagnostic: return type
//! issue: 6076
module Conf;

public interface I
  method P<T, U>(a: T): Array<(T) -> U>;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(a: A): Array<(B) -> A>
    return [];
  end P;
end K;
