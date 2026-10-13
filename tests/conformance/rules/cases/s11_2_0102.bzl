//! case: s11_2_0102
//! doc: spec
//! section: 11.2
//! title: a function-typed parameter with a different parameter type does not match
//! rule: A member matches an interface member when it is declared public, has the same name and arity, the same number of type parameters of its own, and parameter types that are exactly equal under the interface's substitution (and a return type exactly equal under it).
//!   The interface's parameter is (int) -> int; the implementation's (string) -> int is not exactly equal to it.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I
  method P(f: (int) -> int): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P(f: (string) -> int): int
    return 1;
  end P;
end K;
