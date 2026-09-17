//! case: s28_0002
//! doc: spec
//! section: 28
//! title: a record local carries no disposal obligation and draws no discharge diagnostic
//! rule: Records, enums, and primitives need no disposal — their memory is
//!   reclaimed when their containing scope exits; the chapter's summary table
//!   records "Disposal required? No" for each, with no mechanism to announce
//!   (§28).
//! cites: 28, 9.7
//! expect: quiet
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Run()
    var p := new Point(1, 2);
    Console.WriteLine(p.X + p.Y);
  end Run;
end Program;
