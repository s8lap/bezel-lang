//! case: s10_3_0002
//! doc: spec
//! section: 10.3
//! title: short is an admitted underlying type
//! rule: The underlying type must be one of `byte`, `sbyte`, `short`,
//!   `ushort`, `int`, `uint`, `long`, `ulong`.
//! cites: 10.3
//! expect: accept
module Conf;

enum Ratio: short
  Half, Whole;
end Ratio;
