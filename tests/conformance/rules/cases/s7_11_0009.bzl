//! case: s7_11_0009
//! doc: spec
//! section: 7.11
//! title: a relational pattern over a nullable scrutinee is rejected
//! rule: A nullable scrutinee — value or reference — is a compile error at a
//!   relational pattern: ordering admits no absent participant, and pattern
//!   position performs no narrowing.
//! cites: 19.3
//! expect: reject
//! diagnostic: nullable scrutinee
module Conf;

public class Program
  public static method Main()
    var n: int? := int.ParseOrNull("5");
    var result := (case n of
      > 0 => "positive",
      _ => "other");
    Console.WriteLine(result);
  end Main;
end Program;
