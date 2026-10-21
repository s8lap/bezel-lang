//! case: s17_5_0020
//! doc: spec
//! section: 17.5
//! title: an integer constant converts by value across signedness at an in left operand over Array<uint>
//! rule: "The rule is **value-based, not type-based** ... and the target any
//!   integer type whose range contains the value, **across signedness**"
//!   ([§5.1], constant narrowing), so the constant `1` is admissible as
//!   `Contains(item: uint)`'s argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a: Array<uint> := [1, 2];
    var found := 1 in a;
    Console.WriteLine(found);
  end Main;
end Program;
