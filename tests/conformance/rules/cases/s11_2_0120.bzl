//! case: s11_2_0120
//! doc: spec
//! section: 11.2
//! title: an Object return on both sides matches: the accepting twin
//! rule: §11.2: "Return types must match exactly (no covariance in this version)." The exact match holds here: both sides declare Object.
//!   The string literal converts to Object by the §5.1 reference conversion, so the body is admitted.
//! cites: 11.2, 5.1
//! expect: accept
module Conf;

public interface I
  method Get(): Object;
end I;

public class K: I
  public constructor() end constructor;

  public method Get(): Object
    return "hi";
  end Get;
end K;
