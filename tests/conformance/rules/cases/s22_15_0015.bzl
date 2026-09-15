//! case: s22_15_0015
//! doc: spec
//! section: 22.15
//! title: a pure body may not store to a static field
//! rule: Prohibited, as the closed list's contrapositive: any store whose
//!   target root is an instance field, a static field, an indexer slot of
//!   a class instance, or whose access path dereferences a class-typed
//!   reference.
//! cites: 22.15
//! expect: reject
//! diagnostic: static field
module Conf;

public class Counter
  private static _n: int := 0;

  public static pure method Bump()
    _n := _n + 1;
  end Bump;
end Counter;
