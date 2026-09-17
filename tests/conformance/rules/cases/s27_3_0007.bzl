//! case: s27_3_0007
//! doc: spec
//! section: 27.3
//! title: an ascription narrows what a target-typed non-call position admits
//! rule: An ascribed lambda's ascription must agree with the target, judged as
//!   a value's return type under §27.1.1 — so an ascription narrows what the
//!   position admits: `var g: (int) -> long := lambda(x): int => x * 2;` is
//!   rejected where the unascribed form is admitted.
//! cites: 27.3, 27.1.1
//! expect: reject
//! diagnostic: ascription
module Conf;

public class Program
  public method Demo()
    var g: (int) -> long := lambda(x): int => x * 2;
  end Demo;
end Program;
