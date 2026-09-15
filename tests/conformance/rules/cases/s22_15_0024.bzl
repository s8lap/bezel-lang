//! case: s22_15_0024
//! doc: spec
//! section: 22.15
//! title: an override may strengthen purity where the base member is not pure
//! rule: Purity may be strengthened. An override or implementation may
//!   declare `pure` where the overridden or interface member is not.
//! cites: 22.15
//! expect: accept
module Conf;

public class Base
  public virtual method Get(): int => 1;
end Base;

public class Derived: Base
  public pure override method Get(): int => 2;
end Derived;
