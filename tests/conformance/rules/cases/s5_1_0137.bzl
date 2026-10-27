//! case: s5_1_0137
//! doc: spec
//! section: 5.1
//! title: a returned expression whose type does not implicitly convert to the declared return
//!   type is rejected
//! rule: "Between signed and unsigned types of the same or narrower width (e.g. `int -> uint`,
//!   `uint -> int`, `short -> ushort`). Widening from unsigned to a strictly larger signed type
//!   ... is permitted because no value is lost." (§5.1, v0.340). "The top-level operand of
//!   `return expression;` in a body with a declared return type is a **declared-result
//!   position**" (§21.7, v0.340) — the returned expression must convert to the declared return
//!   type. `1 + u` is a uint (§6.2's "the result has the larger type"), and uint does not
//!   convert to int.
//! cites: 5.1, 21.7, 6.2
//! expect: reject
//! diagnostic: implicitly convertible
module Conf;

public class P
  public static method I(u: uint): int => 1 + u;
end P;
