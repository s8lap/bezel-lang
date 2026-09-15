//! case: l2_5_0007
//! doc: stdlib
//! section: 2.5
//! title: a temporal format pattern preserves non-specifier characters literally
//! rule: In a temporal pattern, every character that does not begin one of the
//!   yyyy/MM/dd/HH/mm/ss/fff specifiers is a literal, copied to the output
//!   verbatim.
//! cites: 2.18
//! expect: run
//! stdout: 2024/03/05
module Conf;

public class Program
  public static method Main()
    var d := new DateTime(2024, 3, 5, 0, 0, 0);
    Console.WriteLine(d.ToString("yyyy/MM/dd"));
  end Main;
end Program;
