//! case: s22_8_0003
//! doc: spec
//! section: 22.8
//! title: a readonly field assigned once in the constructor body is admitted
//! rule: A field declared `readonly` may be assigned ... in a constructor
//!   body; the field is also assigned at most once.
//! cites: 22.8
//! expect: accept
module Conf;

public class Counter
  private readonly _start: int;

  public constructor(start: int)
    _start := start;
  end constructor;
end Counter;
