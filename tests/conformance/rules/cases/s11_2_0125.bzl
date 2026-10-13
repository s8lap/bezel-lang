//! case: s11_2_0125
//! doc: spec
//! section: 11.2
//! title: an interface extending a generic interface: the inherited member's substituted return must match
//! rule: §11.2: "Return types must match exactly (no covariance in this version)."
//!   J<T> extends I<T>, so under K: J<int> the inherited member P returns int; the implementation returns string, which is not exactly int.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I<T>
  method P(): T;
end I;

public interface J<T>: I<T>
end J;

public class K: J<int>
  public constructor() end constructor;

  public method P(): string
    return "x";
  end P;
end K;
