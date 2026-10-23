//! case: s35_3_0044
//! doc: spec
//! section: 35.3
//! title: a payload-free case written with a non-empty argument list is rejected
//! rule: A payload case written bare, or a payload-free case written with an argument
//!   list, is the arity error §35.5 names on the pattern side.
//! cites: 35.5
//! expect: reject
//! diagnostic: no payload
module Conf;

public choice Outcome
  Got(b: string),
  Nothing;
end Outcome;

public class Program
  public static method Main()
    var o := Outcome.Nothing(1, 2);   // ERROR: the case has no slots to fill
  end Main;
end Program;
