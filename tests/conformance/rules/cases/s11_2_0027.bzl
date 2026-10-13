//! case: s11_2_0027
//! doc: spec
//! section: 11.2
//! title: a non-generic function-typed argument with a different concrete parameter does not match
//! rule: A member matches an interface member when its parameter types are exactly equal under the interface's substitution, and a member's own type parameters correspond by number and position, not by name. Interface P(a: Array<(int) -> int>) is not matched by P(a: Array<(string) -> int>): the parameter types are concrete and not exactly equal.
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
//! issue: 6053
module Conf;

public interface I
  method P(a: Array<(int) -> int>): int;
end I;

public class K: I
  public constructor() end constructor;

  public method P(a: Array<(string) -> int>): int
    return 1;
  end P;
end K;
