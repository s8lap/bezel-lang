//! case: s17_11_0016
//! doc: spec
//! section: 17.11
//! title: default of a positional record sets every field to its zero value
//! rule: "Records all of whose declared fields are defaultable and which are
//!   not constructor-only: the record value with each field at its zero value.
//!   For positional records, the positional fields are considered."
//! cites: 17.11, 9.1
//! expect: run
//! output: 0
//!   0
module Conf;

public record Pair(First: int, Second: int);

public class Program
  public static method Main()
    var p := default(Pair);
    Console.WriteLine(p.First);
    Console.WriteLine(p.Second);
  end Main;
end Program;
