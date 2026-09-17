//! case: s9_2_0019
//! doc: spec
//! section: 9.2
//! title: override is rejected on a record member other than Equals, GetHashCode, or ToString
//! rule: override is admitted only on Equals, GetHashCode, and ToString, and is
//!   rejected on every other record member, whatever its kind: method, property,
//!   or indexer.
//! expect: reject
//! diagnostic: override
module Conf;

public record Sample
  public readonly property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  public override method Describe(): string => $"Sample({N})";
end Sample;
