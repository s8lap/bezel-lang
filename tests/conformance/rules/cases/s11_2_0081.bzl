//! case: s11_2_0081
//! doc: spec
//! section: 11.2
//! title: a non-generic zero-argument function type with a different concrete return does not match
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P(a: Array<() -> int>) has parameter type Array<() -> int>; the implementation's Array<() -> string> differs in the function's return type.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6053
module Conf;

public interface I
  method P(a: Array<() -> int>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P(a: Array<() -> string>): int
    return 1;
  end P;
end K;
