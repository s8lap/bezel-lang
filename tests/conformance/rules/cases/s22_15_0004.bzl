//! case: s22_15_0004
//! doc: spec
//! section: 22.15
//! title: a pure member declares no out parameter
//! rule: A pure member declares no `ref`, `out`, `out owned`, `owned`,
//!   or `scoped` parameters: `out` obliges a caller-visible assignment
//!   channel.
//! cites: 22.15
//! expect: reject
//! diagnostic: out
module Conf;

public class Ops
  public static pure method TryNegate(x: int, out y: int): bool
    y := -x;
    return true;
  end TryNegate;
end Ops;
