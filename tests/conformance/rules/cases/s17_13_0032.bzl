//! case: s17_13_0032
//! doc: spec
//! section: 17.13
//! title: the discard statement suppresses the unused-result warning on a value result
//! rule: In every permitted form the discard also suppresses the §21.9 unused
//!   result warning ... `_ := ComputeChecksum();` — int: cosmetic — suppresses the
//!   unused-result warning.
//! cites: 17.13, 21.9
//! expect: quiet
module Conf;

public class Ledger
  public method Total(a: int, b: int): int => a + b;

  public method Record(a: int, b: int)
    _ := Total(a, b);
  end Record;
end Ledger;
