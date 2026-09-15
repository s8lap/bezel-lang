//! case: s7_11_0010
//! doc: spec
//! section: 7.11
//! title: separating the null case first admits a relational pattern on the rest
//! rule: The absent case is separated first and the ordering interrogated on
//!   the bound value — `case n of null => …, var m: int when m > 70 => …, _
//!   => … end`.
//! cites: 19.3
//! expect: run
//! stdout: positive
module Conf;

public class Program
  public static method Main()
    var n: int? := 5;
    var result := (case n of
      null => "none",
      var m: int when m > 0 => "positive",
      _ => "other");
    Console.WriteLine(result);
  end Main;
end Program;
