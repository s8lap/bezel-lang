//! case: s10_2_0003
//! doc: spec
//! section: 10.2
//! title: a case naming only one alias of a shared value is exhaustive and legal
//! rule: Exhaustiveness is over the set of distinct declared values, so an
//!   arm naming either alias covers both.
//! cites: 10.2, 19.6
//! expect: accept
module Conf;

enum Level
  Legacy := 1, Current := 1, Retired := 2;
end Level;

public class Program
  public static method Describe(v: Level): string =>
    (case v of
      Legacy => "legacy",
      Retired => "retired");
end Program;
