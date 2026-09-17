//! case: s35_4_0010
//! doc: spec
//! section: 35.4
//! title: a value-like choice is copyable and may be a record field
//! rule: Otherwise the choice is value-like: every payload is a value, a
//!   `string`, a runtime-managed type, or transitively a value-like record or
//!   choice. A value-like choice is an ordinary copyable value type, so section
//!   15.3's move-only record-field ban does not reach it.
//! cites: 15.3
//! expect: accept
module Conf;

public choice Tally
  Counted(n: int),
  Nothing;
end Tally;

public record Box
  public readonly property Content: Tally;

  public constructor(content: Tally)
    Content := content;
  end constructor;

  public method IsNothing(): bool => (case Content of
    Counted(_) => false,
    Nothing    => true);
end Box;
