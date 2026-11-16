//! case: s7_7_0024
//! doc: spec
//! section: 7.7
//! title: ?? over an unconstrained type-parameter operand draws no warning
//! rule: "An operand whose type is a type parameter counts as possibly nullable (§7.10) unless its constraints make it non-nullable there — `where T: notnull`, `value`, `class` or a type bound — so `x ?? d` over an unconstrained `T` is silent, and over a `where T: notnull` `T` it warns."
//! cites: 7.7, 7.10
//! expect: quiet
//! issue: 8596
module Conf;

public class Pick
  public static method OrDefault<T>(x: T, d: T): T
    return x ?? d;
  end OrDefault;
end Pick;
