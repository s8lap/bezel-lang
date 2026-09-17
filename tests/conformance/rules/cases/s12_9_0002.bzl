//! case: s12_9_0002
//! doc: spec
//! section: 12.9
//! title: a covariant type parameter used in input position is rejected
//! rule: out requires T to appear only in output positions... The compiler
//!   enforces position constraints.
//! cites: 12.9
//! expect: reject
//! diagnostic: position
module Conf;

public interface IConsumer<out T>
  method Consume(item: T);
end IConsumer;
