//! case: s22_6_0003
//! doc: spec
//! section: 22.6
//! title: overloads distinguished by the ref modifier are admitted
//! rule: "Overload resolution considers number, types, the `ref`/`out`
//!   modifiers ... — overloads differing in any of these are
//!   distinguishable."
//! cites: 22.6
//! expect: accept
module Conf;

public class Calculator
  public method Compute(a: int): int => a;
  public method Compute(ref a: int): int => a;
end Calculator;
