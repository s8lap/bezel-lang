//! case: s22_15_0025
//! doc: spec
//! section: 22.15
//! title: purity is monotone down an override chain once a member becomes pure
//! rule: Monotone down the chain. An override matches against the
//!   nearest overridden declaration: once a chain member is pure, every
//!   further override is pure.
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
module Conf;

public class Base
  public pure virtual method Get(): int => 1;
end Base;

public class Mid: Base
  public pure override method Get(): int => 2;
end Mid;

public class Leaf: Mid
  public override method Get(): int => 3;
end Leaf;
