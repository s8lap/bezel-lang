//! case: s35_3_0039
//! doc: spec
//! section: 35.3
//! title: the same payload-free case in an interpolation hole, written as a value, is admitted
//! rule: Construction is qualified ... `FetchOutcome.NotModified` for a payload-free
//!   case (a value expression, not a call). The hole is an ordinary expression position,
//!   so the qualified value form is what appears there.
//! cites: 18.5
//! expect: accept
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Label
  public static method Render(): string
    return $"x{Outcome.Nothing}";
  end Render;
end Label;
