//! case: s12_9_0010
//! doc: spec
//! section: 12.9
//! title: a contravariant type parameter in a covariant base's argument is rejected
//! rule: The extending interface instantiates the base's variant parameter
//!   with its own type parameter, holding the deriving interface's variance.
//! cites: 12.9
//! expect: reject
//! diagnostic: base type
//! issue: 6348
module Conf;

public interface Bad<in T>: IEnumerable<T>
end Bad;
