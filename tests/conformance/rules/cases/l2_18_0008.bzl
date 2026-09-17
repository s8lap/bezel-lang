//! case: l2_18_0008
//! doc: stdlib
//! section: 2.18
//! title: a format spec on char is a compile error
//! rule: bool, char, string take none of the specifier vocabulary — a
//!   format spec on these is a compile error.
//! cites: 2.18
//! expect: reject
//! diagnostic: format
module Conf;

public class Program
  public static method Main()
    var c := 'a';
    Console.WriteLine($"{c:X}");
  end Main;
end Program;
