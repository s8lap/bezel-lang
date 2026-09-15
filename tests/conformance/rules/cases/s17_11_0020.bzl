//! case: s17_11_0020
//! doc: spec
//! section: 17.11
//! title: a computed property does not affect a record's default eligibility
//! rule: "Expression-bodied (computed) properties have no backing storage and
//!   do not affect `default` eligibility."
//! cites: 17.11, 23.2, 9.2
//! expect: run
//! output: 0
//!   span
module Conf;

public record Span(Start: int, Stop: int)
  public property Label: string => "span";
end Span;

public class Program
  public static method Main()
    var s := default(Span);
    Console.WriteLine(s.Start);
    Console.WriteLine(s.Label);
  end Main;
end Program;
