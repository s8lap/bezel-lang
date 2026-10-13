//! case: s11_2_0016
//! doc: spec
//! section: 11.2
//! title: a bare function-typed return with its own type parameters swapped by position is rejected
//! rule: Return types must match exactly under the interface's substitution, and a member's own type parameters correspond by number and position. Interface P<T, U>(): (T) -> U is not matched by P<A, B>(): (B) -> A.
//! cites: 11.2
//! expect: reject
//! diagnostic: return type
//! issue: 6092
module Conf;

public interface I
  method P<T, U>(): (T) -> U;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(): (B) -> A
    throw new Exception("x");
  end P;
end K;
