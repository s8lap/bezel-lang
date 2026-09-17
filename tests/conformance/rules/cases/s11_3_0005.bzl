//! case: s11_3_0005
//! doc: spec
//! section: 11.3
//! title: methods distinguished by parameter type are legal overloads, not a clash
//! rule: Legal overloads -- members of the same name that are distinguishable
//!   per §22.6 -- are unaffected; this rule rejects only the duplicates that
//!   overloading does not admit.
//! cites: 11.3, 22.6
//! expect: accept
module Conf;

public class Calculator
  public constructor()
  end constructor;

  public method Add(a: int, b: int): int => a + b;
  public method Add(a: double, b: double): double => a + b;
end Calculator;
