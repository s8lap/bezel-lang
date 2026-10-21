//! case: s17_5_0019
//! doc: spec
//! section: 17.5
//! title: an integer constant that does not fit the Set<byte> element range is rejected at an in left operand
//! rule: "A constant whose value does not fit the target's range is a
//!   **compile error** at the conversion site — never a silent truncation"
//!   ([§5.1], constant narrowing), and the left operand must be admissible as
//!   `Contains(item: byte)`'s argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: does not fit
module Conf;

public class Program
  public static method Main()
    var s: Set<byte> := [1, 2];
    var found := 300 in s;
    Console.WriteLine(found);
  end Main;
end Program;
