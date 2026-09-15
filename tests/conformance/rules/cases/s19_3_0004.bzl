//! case: s19_3_0004
//! doc: spec
//! section: 19.3
//! title: a range pattern over a nullable scrutinee is rejected
//! rule: The nullable-scrutinee rejection at a relational pattern applies
//!   alike at a range pattern; the rejection is keyed on the kind of
//!   pattern, not on which bounds are written.
//! cites: 19.3, 16.6
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Main()
    var n: int? := 80;
    case n of
      70..100 => Console.WriteLine("high");,
      _       => Console.WriteLine("low");
    end case;
  end Main;
end Program;
