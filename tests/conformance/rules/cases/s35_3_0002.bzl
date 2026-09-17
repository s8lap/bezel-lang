//! case: s35_3_0002
//! doc: spec
//! section: 35.3
//! title: a bare unqualified case name outside a declared-result position is rejected
//! rule: In expression position outside a declared-result position the qualified form
//!   is required, exactly as for enum constants outside their scrutinee.
//! expect: reject
//! diagnostic: qualified form
module Conf;

public choice Outcome
  Ready,
  Empty;
end Outcome;

public class Program
  public static method Main()
    var o := Ready;   // ERROR: bare case name outside a declared-result position
  end Main;
end Program;
