//! case: s12_10_0003
//! doc: spec
//! section: 12.10
//! title: an interface extending itself is a cycle
//! rule: An inheritance cycle is a compile error. A type may not, directly or
//!   transitively, list itself among its base types — neither as the base
//!   class of a class, nor as an extended interface of an interface.
//! cites: 12.10, C.1
//! expect: reject
//! diagnostic: cycle
module Conf;

public interface IThing: IThing
  method Ping(): int;
end IThing;
