//! case: s22_15_0003
//! doc: spec
//! section: 22.15
//! title: a pure member declares no ref parameter
//! rule: A pure member declares no `ref`, `out`, `out owned`, `owned`,
//!   or `scoped` parameters: `ref` is a writable alias of caller storage.
//! cites: 22.15
//! expect: reject
//! diagnostic: ref
module Conf;

public class Ops
  public static pure method Negate(ref x: int): int => -x;
end Ops;
