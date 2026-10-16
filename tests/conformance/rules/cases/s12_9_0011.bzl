//! case: s12_9_0011
//! doc: spec
//! section: 12.9
//! title: the same base instantiation is admitted under a covariant marker
//! rule: The extending interface instantiates the base's variant parameter
//!   with its own type parameter, holding the deriving interface's variance.
//! cites: 12.9
//! expect: accept
//! issue: 6348
module Conf;

public interface Good<out T>: IEnumerable<T>
end Good;
