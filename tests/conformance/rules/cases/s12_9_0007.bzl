//! case: s12_9_0007
//! doc: spec
//! section: 12.9
//! title: a covariant type parameter under a covariant interface wrapper is admitted
//! rule: Position is composed across nested type constructors. A type parameter
//!   inside another generic type inherits position from that wrapper's
//!   variance — `IEnumerable` is `out T`, so in a return type the composed
//!   position stays covariant.
//! cites: 12.9
//! expect: accept
//! issue: 6348
module Conf;

public interface I<out T>
  method Get(): IEnumerable<T>;
end I;
