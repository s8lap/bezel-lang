//! case: s12_9_0001
//! doc: spec
//! section: 12.9
//! title: a covariant type parameter used only in output position is admitted
//! rule: out requires T to appear only in output positions (return types,
//!   read-only property types, out parameters).
//! cites: 12.9
//! expect: accept
module Conf;

public interface IProducer<out T>
  method Produce(): T;
end IProducer;
