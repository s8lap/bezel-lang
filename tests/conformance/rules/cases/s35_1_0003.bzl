//! case: s35_1_0003
//! doc: spec
//! section: 35.1
//! title: duplicate case names within one choice are rejected
//! rule: Case names are PascalCase by convention and must be distinct within the choice.
//! expect: reject
//! diagnostic: distinct
module Conf;

public choice Outcome
  Empty,
  Empty;   // ERROR: `Empty` declared twice
end Outcome;
