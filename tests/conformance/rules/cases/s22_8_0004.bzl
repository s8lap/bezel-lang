//! case: s22_8_0004
//! doc: spec
//! section: 22.8
//! title: a readonly field with an initializer assigned again in the constructor body is rejected
//! rule: "A field initializer and a constructor-body assignment are
//!   therefore **two** assignments wherever both run, which is wherever
//!   the constructor's chaining position holds `base(...)` or the implicit
//!   `base()`."
//! cites: 22.8
//! expect: reject
//! diagnostic: possibly already assigned
module Conf;

public class Counter
  private readonly _start: int := 0;

  public constructor(start: int)
    _start := start;
  end constructor;
end Counter;
