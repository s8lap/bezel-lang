//! case: s11_2_0121
//! doc: spec
//! section: 11.2
//! title: a nullable return type does not match a non-nullable interface return
//! rule: §11.2: "Return types must match exactly (no covariance in this version)."
//!   The interface returns int and the implementation returns int?. The nullability widening in §11.2 is stated for parameters only.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I
  method Find(): int;
end I;

public class K: I
  public constructor() end constructor;

  public method Find(): int?
    return 1;
  end Find;
end K;
