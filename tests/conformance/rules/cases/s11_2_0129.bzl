//! case: s11_2_0129
//! doc: spec
//! section: 11.2
//! title: a dropped generic-method constraint does not match the interface
//! rule: §11.2: "and, for a generic method, a constraint set equivalent to the interface method's (§14.2)."
//!   The interface method constrains its own type parameter U with new(); the implementation's V carries no constraint, so the constraint sets differ.
//! cites: 11.2, 14.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I
  method Count<U>(): int where U: new();
end I;

public class K: I
  public constructor() end constructor;

  public method Count<V>(): int
    return 1;
  end Count;
end K;
