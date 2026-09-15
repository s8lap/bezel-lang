//! case: s19_3_0008
//! doc: spec
//! section: 19.3
//! title: range, relational, and constant patterns select by value in order
//! rule: A leading `-` on a numeric literal is part of the constant
//!   pattern; ranges are inclusive (`90..100`) or exclusive (`0..<60`),
//!   open-ended (`31..`, `..<0`), or fully open (`..`); a relational
//!   pattern compares under the scrutinee's ordering; arms are tried in
//!   textual order and the first match wins.
//! cites: 19.3, 19.4
//! expect: run
//! stdout: strongly down
module Conf;

public class Program
  public static method Classify(delta: int): string => (case delta of
    -10..-6 => "strongly down",
    ..<0    => "down",
    0       => "flat",
    > 100   => "way up",
    31..    => "up",
    ..      => "steady");

  public static method Main()
    Console.WriteLine(Classify(-8));
    Console.WriteLine(Classify(-1));
    Console.WriteLine(Classify(0));
    Console.WriteLine(Classify(200));
    Console.WriteLine(Classify(50));
    Console.WriteLine(Classify(10));
  end Main;
end Program;
