//! case: s11_2_0031
//! doc: spec
//! section: 11.2
//! title: a multi-parameter function-typed return with the parameters swapped by position does not match
//! rule: Return types must match exactly under the interface's substitution. Interface P<T, U>(): (T, U) -> T is not matched by P<A, B>(): (B, A) -> B, because the own type parameters correspond by position, not by name.
//! cites: 11.2
//! expect: reject
//! diagnostic: return type
//! issue: 6092
module Conf;

public interface I
  method P<T, U>(): (T, U) -> T;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(): (B, A) -> B
    throw new Exception("x");
  end P;
end K;
