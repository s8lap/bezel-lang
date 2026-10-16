//! case: s12_9_0006
//! doc: spec
//! section: 12.9
//! title: a covariant type parameter under an invariant class wrapper is rejected
//! rule: Position is composed across nested type constructors. A type parameter
//!   inside another generic type inherits position from that wrapper's
//!   variance; classes, records, and concrete generic types are always
//!   invariant, so no marker survives one.
//! cites: 12.9
//! expect: reject
//! diagnostic: position
//! issue: 6348
module Conf;

public interface I<out T>
  method Get(): Array<T>;
end I;
