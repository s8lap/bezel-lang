//! case: s9_6_0004
//! doc: spec
//! section: 9.6
//! title: a record may implement an interface whose members are all read-only or methods
//! rule: A record may implement any number of interfaces that declare no
//!   settable property or indexer member.
//! cites: 9.4
//! expect: accept
module Conf;

public interface IPointLike
  readonly property X: int;
  method Describe(): string;
end IPointLike;

public record Point2(X: int): IPointLike
  public method Describe(): string => $"({X})";
end Point2;
