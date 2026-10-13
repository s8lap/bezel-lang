//! case: s11_2_0115
//! doc: spec
//! section: 11.2
//! title: a property whose substituted type is not matched by the implementation is rejected
//! rule: A read-write member satisfies a read-only interface member of the same name and type (§11.2).
//!   A member matches only when its type is exactly equal to the interface member's type under the interface's substitution.
//!   Under I<int> the property type is int; the implementation declares Value as string, which is not exactly int.
//! issue: 6130
//! cites: 11.2
//! expect: reject
//! diagnostic: does not match
module Conf;

public interface I<T>
  readonly property Value: T;
end I;

public class K: I<int>
  public constructor() end constructor;

  public property Value: string := "x";
end K;
