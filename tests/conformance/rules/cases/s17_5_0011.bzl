//! case: s17_5_0011
//! doc: spec
//! section: 17.5
//! title: an int tests membership in a range literal, the spec's own example
//! rule: "`x in collection` evaluates to `true` if `collection.Contains(x)` is
//!   true", with the example `var inRange := score in 60..100;` ([§17.5]).
//!   A range's `Contains` takes its non-nullable element type ([§16.3]).
//! cites: 17.5, 16.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var score: int := 75;
    var inRange := score in 60..100;
    Console.WriteLine(inRange);
  end Main;
end Program;
