//! case: s17_5_0024
//! doc: spec
//! section: 17.5
//! title: an int left operand of the element type is admitted into a Set<int>
//! rule: "**identity**: every type converts to itself" ([§5.1]), so the int
//!   left operand is admissible as `Contains(item: int)`'s argument, and the
//!   membership test runs ([§17.5]).
//! cites: 17.5, 5.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var s: Set<int> := [1, 2];
    var i: int := 1;
    var found := i in s;
    Console.WriteLine(found);
  end Main;
end Program;
