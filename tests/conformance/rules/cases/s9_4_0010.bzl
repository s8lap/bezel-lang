//! case: s9_4_0010
//! doc: spec
//! section: 9.4
//! title: a store to a positional record's auto-generated property is rejected
//! rule: Properties auto-generated from positional fields are read-only. A
//!   record declared only with positional fields is immutable.
//! cites: 9.1
//! expect: reject
//! diagnostic: read-only
//! notes: twin s9_4_0005 — the same external store through a named binding
//!   is legal on a settable body-form auto-property.
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Move(): int
    var p := new Point(1, 2);
    p.X := 5;
    return p.X;
  end Move;
end Program;
