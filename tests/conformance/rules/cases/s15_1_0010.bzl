//! case: s15_1_0010
//! doc: spec
//! section: 15.1
//! title: the non-nullable Pop on a Stack of an inline array is admitted and runs
//! rule: a compile error at the use ... naming the member and the remedies, a
//!   count or index test, the non-nullable member, or an element type that
//!   wraps the array in a record
//! cites: 15.1, 14.2
//! expect: run
//! output: 7
//!   1
module Conf;

public class Program
  public static method Main()
    using var s := new Stack<[3]int>();
    var a: [3]int := [1, 2, 3];
    s.Push(a);
    var b: [3]int := [7, 8, 9];
    s.Push(b);
    var top := s.Pop();
    Console.WriteLine(top[0]);
    Console.WriteLine(s.Count);
  end Main;
end Program;
