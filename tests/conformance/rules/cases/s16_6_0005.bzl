//! case: s16_6_0005
//! doc: spec
//! section: 16.6
//! title: v in r over a nullable v is rejected for every range test
//! rule: "`v in r` over a nullable `v` is rejected alike [as the range-pattern
//!   case], both for one reason: range membership is an ordering test, and
//!   ordering admits no absent participant... so `n in 1..5` over an `int?` is
//!   rejected."
//! cites: 16.6, 16.3, 17.5
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Main()
    var n: int? := int.ParseOrNull("5");
    var b := n in 1..10;
  end Main;
end Program;
