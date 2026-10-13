//! case: s11_2_0033
//! doc: spec
//! section: 11.2
//! title: a nullable function-typed return with own type parameters swapped by position does not match
//! rule: Return types must match exactly under the interface's substitution. Interface P<T, U>(): ((T) -> U)? is not matched by P<A, B>(): ((B) -> A)?, because the own type parameters correspond by position, not by name.
//! cites: 11.2
//! expect: reject
//! diagnostic: return type
//! issue: 6092
module Conf;

public interface I
  method P<T, U>(): ((T) -> U)?;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(): ((B) -> A)?
    throw new Exception("x");
  end P;
end K;
