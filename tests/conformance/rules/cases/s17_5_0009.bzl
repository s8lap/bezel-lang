//! case: s17_5_0009
//! doc: spec
//! section: 17.5
//! title: a long left operand does not implicitly convert to an Array<int> element
//! rule: "A value of one type implicitly converts to another when no
//!   programmer-visible information is lost" ([§5.1]); `long` to `int` loses
//!   information and has no row in the §5.1 table, so the left operand is not
//!   admissible as `Contains(item: int)`'s argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: implicitly convert
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2];
    var i: long := 2;
    var found := i in a;
    Console.WriteLine(found);
  end Main;
end Program;
