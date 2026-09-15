//! case: s22_8_0002
//! doc: spec
//! section: 22.8
//! title: a readonly field assigned in an ordinary method is rejected
//! rule: A field declared `readonly` may be assigned only in a field
//!   initializer or in a constructor body; assignment anywhere else is a
//!   compile error.
//! cites: 22.8
//! expect: reject
//! diagnostic: readonly
module Conf;

public class Counter
  private readonly _start: int;

  public constructor(start: int)
    _start := start;
  end constructor;

  public method Reset()
    _start := 0;
  end Reset;
end Counter;
