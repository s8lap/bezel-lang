//! case: s22_5_0007
//! doc: spec
//! section: 22.5
//! title: a default value may read an earlier parameter, never a later one
//! rule: A default-value expression may "read other parameters appearing
//!   **earlier** in the same parameter list (e.g. `M(a: int, b: int := a +
//!   1)` is permitted; later-parameter forward references are not)."
//! cites: 22.5
//! expect: reject
//! diagnostic: earlier
module Conf;

public class Calc
  public static method Add(a: int := b, b: int := 1): int => a + b;
end Calc;
