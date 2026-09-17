//! case: s16_2_0002
//! doc: spec
//! section: 16.2
//! title: open-ended range patterns match "less than" and "at or above" the written bound
//! rule: "In a pattern, `..<0` matches 'any value less than 0' and `31..` matches
//!   'any value at or above 31.'"
//! cites: 16.2, 19.3
//! expect: run
//! stdout: negative
module Conf;

public class Program
  public static method Classify(x: int): string => (case x of
    ..<0  => "negative",
    31..  => "high",
    _     => "mid");

  public static method Main()
    Console.WriteLine(Classify(-5));
    Console.WriteLine(Classify(40));
    Console.WriteLine(Classify(10));
  end Main;
end Program;
