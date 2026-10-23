//! case: s5_1_0138
//! doc: spec
//! section: 5.1
//! title: the same returned expression is admitted where the declared return type is long
//! rule: "Widening from unsigned to a strictly larger signed type (e.g. `byte -> short`,
//!   `ushort -> int`) is permitted because no value is lost." (§5.1, v0.340). The returned
//!   expression is a uint (§6.2's "the result has the larger type"), and uint widens to long,
//!   so the declared-result position (§21.7) is satisfied.
//! cites: 5.1, 21.7, 6.2
//! expect: accept
module Conf;

public class P
  public static method I(u: uint): long => 1 + u;
end P;
