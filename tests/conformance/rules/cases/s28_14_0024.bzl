//! case: s28_14_0024
//! doc: spec
//! section: 28.14
//! title: a method reference over Add at Array<() -> int> is rejected at the formation
//! rule: Forming a method reference is likewise a compile error when a parameter
//!   slot of the formed function type is, nullability stripped, a function
//!   type and binds to a callee parameter, other than an `out` parameter,
//!   that is a set-erasing position (`var add := xs.Add;` at `xs: Array<() ->
//!   int>`; ...)
//! cites: 28.14, 28.3, 27.6
//! expect: reject
//! diagnostic: method reference
//! issue: 5515
module Conf;

public class Lists
  public static method Form(): int
    using var xs := new Array<() -> int>();
    var add := xs.Add;
    return xs.Count;
  end Form;
end Lists;
