//! case: s9_2_0015
//! doc: spec
//! section: 9.2
//! title: virtual is rejected on a record member
//! rule: A record member may not be declared virtual, abstract, or sealed
//!   override (v0.297); record members are always dispatched statically.
//! expect: reject
//! diagnostic: virtual
module Conf;

public record Sample
  public readonly property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  public virtual method Describe(): string => $"Sample({N})";
end Sample;
