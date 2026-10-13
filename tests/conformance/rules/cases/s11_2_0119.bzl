//! case: s11_2_0119
//! doc: spec
//! section: 11.2
//! title: a covariant string return does not match an Object return
//! rule: §11.2: "Return types must match exactly (no covariance in this version)."
//!   The interface returns Object and the implementation returns string, a subtype of Object.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I
  method Get(): Object;
end I;

public class K: I
  public constructor() end constructor;

  public method Get(): string
    return "hi";
  end Get;
end K;
