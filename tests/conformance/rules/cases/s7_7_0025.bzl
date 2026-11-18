//! case: s7_7_0025
//! doc: spec
//! section: 7.7
//! title: ?? over a where T: notnull type-parameter operand warns that the right operand is never evaluated
//! rule: "An operand whose type is a type parameter counts as possibly nullable (§7.10) unless its constraints make it non-nullable there — `where T: notnull`, `value`, `class` or a type bound — so `x ?? d` over an unconstrained `T` is silent, and over a `where T: notnull` `T` it warns."
//!   Twin of s7_7_0024: the constraint is the one difference.
//! cites: 7.7, 7.10, 14.2
//! expect: warn
//! diagnostic: never evaluated
//! issue: 8596
module Conf;

public class Pick
  public static method OrDefault<T>(x: T, d: T): T where T: notnull
    return x ?? d;
  end OrDefault;
end Pick;
