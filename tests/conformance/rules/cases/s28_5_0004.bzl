//! case: s28_5_0004
//! doc: spec
//! section: 28.5
//! title: owned on a function-typed field is rejected
//! rule: Function-typed fields may not be `owned`. Function values are
//!   runtime-managed.
//! cites: 28.1, 28.5, 28.9
//! expect: reject
//! diagnostic: function-typed
module Conf;

public class Registry
  private owned _cb: (int) -> int := lambda(x: int): int => x;
end Registry;
