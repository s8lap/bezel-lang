//! case: s10_2_0002
//! doc: spec
//! section: 10.2
//! title: two case arms naming same-valued constants is rejected
//! rule: In a `case` over the enum, two arms — or two `|` alternatives —
//!   naming same-valued constants are a compile error, the later one being
//!   unreachable.
//! cites: 10.2, 19.6
//! expect: reject
//! diagnostic: unreachable
module Conf;

enum Level
  Legacy := 1, Current := 1, Retired := 2;
end Level;

public class Program
  public static method Describe(v: Level): string =>
    (case v of
      Legacy => "legacy",
      Current => "current",
      Retired => "retired");
end Program;
