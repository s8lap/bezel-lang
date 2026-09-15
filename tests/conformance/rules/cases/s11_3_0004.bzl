//! case: s11_3_0004
//! doc: spec
//! section: 11.3
//! title: two methods with an identical signature clash
//! rule: Two members clash when they have the same name... two methods...
//!   with the same name that are not distinguishable as overloads.
//! cites: 11.3, 22.6
//! expect: reject
//! diagnostic: clash
module Conf;

public class Calculator
  public constructor()
  end constructor;

  public method Add(a: int, b: int): int => a + b;
  public method Add(a: int, b: int): int => a + b;
end Calculator;
