//! case: s17_5_0026
//! doc: spec
//! section: 17.5
//! title: a char left operand widens to int for membership in an Array<int>
//! rule: The §5.1 table lists `char` as implicitly converting to `int`
//!   ([§5.1]), so the left operand is admissible as `Contains(item: int)`'s
//!   argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [97];
    var c: char := 'a';
    var found := c in a;
    Console.WriteLine(found);
  end Main;
end Program;
