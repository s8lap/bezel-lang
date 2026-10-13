//! case: s11_2_0105
//! doc: spec
//! section: 11.2
//! title: an inline-array parameter of a different length does not match
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   Parameter types must be exactly equal under the interface's substitution; [3]int and [2]int are not equal.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I
  method P(a: [3]int): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P(a: [2]int): int
    return 1;
  end P;
end K;
