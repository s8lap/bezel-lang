//! case: s22_15_0014
//! doc: spec
//! section: 22.15
//! title: a pure body may not store to an instance field outside a pure constructor's birth store
//! rule: Prohibited, as the closed list's contrapositive: any store whose
//!   target root is an instance field ... (stores to `this`'s fields
//!   outside pure constructors' birth stores included).
//! cites: 22.15
//! expect: reject
//! diagnostic: instance field
module Conf;

public class Box
  private _v: int := 0;

  public pure method Reset()
    _v := 0;
  end Reset;
end Box;
