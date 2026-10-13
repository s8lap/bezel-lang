//! case: s11_2_0011
//! doc: spec
//! section: 11.2
//! title: a function-typed argument with a different arity does not match
//! rule: Parameter types must match exactly under the interface's substitution. Interface P<T>(a: Array<(T) -> T>) is not matched by P<X>(a: Array<(X, X) -> X>): a two-parameter function type is a different type from a one-parameter function type.
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

  public method P<X>(a: Array<(X, X) -> X>): int
    return 1;
  end P;
end K;
