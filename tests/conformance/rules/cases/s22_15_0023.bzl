//! case: s22_15_0023
//! doc: spec
//! section: 22.15
//! title: an override of a pure member must itself be declared pure
//! rule: Purity is never dropped. An override of a pure member must
//!   itself be declared `pure`, and a class member implementing a pure
//!   interface member must be declared `pure`; omission is a compile
//!   error at the override or implementation.
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
module Conf;

public class Base
  public virtual pure method Get(): int => 1;
end Base;

public class Derived: Base
  public override method Get(): int => 2;
end Derived;
