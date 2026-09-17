//! case: s17_13_0031
//! doc: spec
//! section: 17.13
//! title: an undiscarded int-returning call draws the unused-result warning
//! rule: In every permitted form the discard also suppresses the §21.9 unused
//!   result warning **where the undiscarded expression statement would draw one**.
//! cites: 17.13, 21.9
//! expect: warn
//! diagnostic: discarded
module Conf;

public class Ledger
  public method Total(a: int, b: int): int => a + b;

  public method Record(a: int, b: int)
    Total(a, b);
  end Record;
end Ledger;
