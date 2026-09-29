//! case: s16_6_0003
//! doc: spec
//! section: 16.6
//! title: a range pattern rejects a nullable scrutinee
//! rule: "a range pattern rejects a nullable scrutinee — keyed on the pattern's
//!   kind, the open-ended and fully open forms with the closed ones ... range
//!   membership is an ordering test, and ordering admits no absent participant."
//! cites: 16.6, 16.3, 19.3
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Main()
    var x: int? := int.ParseOrNull("5");
    case x of
      1..10 => Console.WriteLine("in range"),
      _     => Console.WriteLine("other")
    end case;
  end Main;
end Program;
