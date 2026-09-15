//! case: s22_12_0009
//! doc: spec
//! section: 22.12
//! title: a static readonly field of a synchronized handle type is admitted warning-free
//! rule: declares synchronized, not displacement-unbounded — the form-(i)
//!   set — `static readonly`: ok — in the set.
//! cites: 22.12
//! expect: accept
module Conf;

public class Registry
  private static readonly _lock: Mutex := new Mutex();
end Registry;
