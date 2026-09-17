//! case: s11_2_0006
//! doc: spec
//! section: 11.2
//! title: a record may not implement an interface declaring a settable member
//! rule: A record may implement any number of interfaces that declare no
//!   settable property or indexer member -- a conformance naming an
//!   interface with one is rejected at the declaration (a value kind
//!   supplies no receiver-storing accessor).
//! cites: 11.2, 9.4, 9.6
//! expect: reject
//! diagnostic: settable
module Conf;

public interface IMutablePoint
  property X: int;
end IMutablePoint;

public record Point(X: int): IMutablePoint;
