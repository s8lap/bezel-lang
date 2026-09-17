//! case: s9_2_0017
//! doc: spec
//! section: 9.2
//! title: sealed override is rejected on a record member
//! rule: A record member may not be declared virtual, abstract, or sealed
//!   override (v0.297); bare sealed on any member is already section 12.4's
//!   error, and this rule adds sealed override.
//! cites: 12.4
//! expect: reject
//! diagnostic: sealed
module Conf;

public record Sample
  public readonly property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  public sealed override method ToString(): string => $"Sample({N})";
end Sample;
