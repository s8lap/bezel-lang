//! case: s17_5_0022
//! doc: spec
//! section: 17.5
//! title: a compile-time constant of type long narrows by value at an in left operand over Array<int>
//! rule: "An expression of integer type that is a compile-time constant ([§17.14])
//!   implicitly converts to any integer type whose range contains its value"
//!   and "the source may be a compile-time constant of **any** integer type —
//!   not only `int`" ([§5.1], constant narrowing), so the left operand is
//!   admissible as `Contains(item: int)`'s argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    const K: long := 2;
    var a: Array<int> := [1, 2];
    var found := K in a;
    Console.WriteLine(found);
  end Main;
end Program;
