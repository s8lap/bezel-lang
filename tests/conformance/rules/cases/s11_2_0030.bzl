//! case: s11_2_0030
//! doc: spec
//! section: 11.2
//! title: a function-typed return with an own type parameter replaced by a concrete type does not match
//! rule: Return types must match exactly under the interface's substitution. Interface P<T, U>(a: T): Array<(T) -> U> is not matched by P<A, B>(a: A): Array<(A) -> int>: the function type's return is the own parameter U on the interface and the concrete int on the implementation.
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

  public method P<A, B>(a: A): Array<(A) -> int>
    return [];
  end P;
end K;
