//! case: s28_9_0008
//! doc: spec
//! section: 28.9
//! title: using var on a function-typed local still forbids reassignment, though disposal there is a no-op
//! rule: A `using var` local of function type is nonetheless permitted
//!   (disposal at scope end is a no-op) and serves only the
//!   non-reassignability role — a plain `var` of function type scopes its
//!   captures identically.
//! cites: 28.9, 28.10
//! expect: reject
//! diagnostic: reassign
module Conf;

public class Program
  public static method Main()
    using var cb: () -> int := lambda() => 1;
    cb := lambda() => 2;
  end Main;
end Program;
