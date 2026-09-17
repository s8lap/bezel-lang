//! case: s9_6_0003
//! doc: spec
//! section: 9.6
//! title: a record cannot implement an interface that declares a settable property
//! rule: A record may implement any number of interfaces that declare no
//!   settable property or indexer member; a conformance naming an interface with
//!   one is rejected at the declaration, since a value kind supplies no
//!   receiver-storing accessor.
//! cites: 9.4
//! expect: reject
//! diagnostic: settable
module Conf;

public interface IMutablePoint
  property X: int;
end IMutablePoint;

public record Point2(X: int): IMutablePoint;
