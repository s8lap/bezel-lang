//! case: s11_2_0089
//! doc: spec
//! section: 11.2
//! title: a renamed nullable bare function-typed return matches
//! rule: Return types must match exactly under the interface's substitution, and own type parameters correspond by number and position, not by name. Interface P<T, U>(): ((T) -> U)? is matched by P<A, B>(): ((A) -> B)?: the nullable wrapper is the same and the function type's own parameters correspond by position.
//! cites: 11.2
//! expect: accept
//! issue: 6092
module Conf;

public interface I
  method P<T, U>(): ((T) -> U)?;
end I;

public class K: I
  public constructor() end constructor;

  public method P<A, B>(): ((A) -> B)?
    throw new Exception("x");
  end P;
end K;
