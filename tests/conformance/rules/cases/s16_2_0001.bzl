//! case: s16_2_0001
//! doc: spec
//! section: 16.2
//! title: a range may omit either endpoint, or both, to construct an open-ended range
//! rule: "A range may omit one endpoint, producing an open-ended range used for
//!   slicing and pattern matching" — the four forms `a..b`, `..b`, `a..`, `..`
//!   are all admitted range expressions.
//! cites: 16.2, 16.3
//! expect: accept
module Conf;

public class Program
  private static method Build()
    var closedRange: Range<int> := 2..5;
    var lowerOpen: Range<int> := ..5;
    var upperOpen: Range<int> := 10..;
    var fullyOpen: Range<int> := ..;
  end Build;
end Program;
