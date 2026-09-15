//! case: s22_8_0005
//! doc: spec
//! section: 22.8
//! title: a readonly field assigned in mutually exclusive branches is admitted
//! rule: Assignments in mutually exclusive branches are one assignment
//!   per path and are admitted.
//! cites: 22.8
//! expect: accept
module Conf;

public class Counter
  private readonly _start: int;

  public constructor(useDefault: bool, start: int)
    if useDefault then
      _start := 0;
    else
      _start := start;
    end if;
  end constructor;
end Counter;
