//! case: s11_2_0109
//! doc: spec
//! section: 11.2
//! title: a candidate with the same number of own type parameters matches a generic interface member
//! rule: A candidate declaring a different number of type parameters of its own does not match.
//!   The accepting twin of s11_2_0108: the candidate declares one own type parameter at the same position, so it matches Show<U>(x: U): string.
//! cites: 11.2
//! expect: accept
module Conf;

public interface I
  method Show<U>(x: U): string;
end I;

public class K: I
  public constructor() end constructor;

  public method Show<T>(x: T): string
    return "a";
  end Show;
end K;
