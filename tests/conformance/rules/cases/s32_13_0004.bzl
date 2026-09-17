//! case: s32_13_0004
//! doc: spec
//! section: 32.13
//! title: a single-field record over a double is not admitted directly by value
//! rule: Float/double single-field records join Stage D; `bool` is excluded to avoid
//!   pinning an aggregate-normalization rule nothing needs.
//! cites: 32.13, 32.14
//! expect: reject
//! diagnostic: single-field
module Conf;

public record Measurement(Value: double);

extern "libexample"
  method Report(m: Measurement): int;
end;
