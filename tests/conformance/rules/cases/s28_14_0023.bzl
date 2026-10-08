//! case: s28_14_0023
//! doc: spec
//! section: 28.14
//! title: a method reference over TryGetValue at a function-typed value type is admitted
//! rule: Forming a method reference is likewise a compile error when a parameter
//!   slot of the formed function type is, nullability stripped, a function
//!   type and binds to a callee parameter, other than an `out` parameter,
//!   that is a set-erasing position ... An `out` parameter is exempt (`var tg
//!   := d.TryGetValue;` at `d: Dictionary<string, () -> int>` is admitted): a
//!   call through the formed value passes it a location and no value, and
//!   what the callee copies out is capture-free
//! cites: 28.14, 28.3, 27.6
//! expect: accept
module Conf;

public class Lookup
  public static method Probe(): bool
    using var d := new Dictionary<string, () -> int>();
    var tg := d.TryGetValue;
    return tg("k", out var g);
  end Probe;
end Lookup;
