//! case: s22_16_0009
//! doc: spec
//! section: 22.16
//! title: result is a compile error in a requires clause
//! rule: `result` is a compile error ... in any `requires` or
//!   `invariant` clause.
//! cites: 22.16
//! expect: reject
//! diagnostic: result
module Conf;

public class Ops
  public static method Weird(x: int): int
    requires(result > 0)

    return x;
  end Weird;
end Ops;
