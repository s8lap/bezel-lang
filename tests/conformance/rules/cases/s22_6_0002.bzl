//! case: s22_6_0002
//! doc: spec
//! section: 22.6
//! title: overloads distinguished by arity are admitted
//! rule: "Overload resolution considers number, types, the `ref`/`out`
//!   modifiers, and the presence of `owned` on an ordinary parameter —
//!   overloads differing in any of these are distinguishable."
//! cites: 22.6
//! expect: accept
module Conf;

public class Calculator
  public method Compute(a: int): int => a;
  public method Compute(a: int, b: int): int => a + b;
end Calculator;
