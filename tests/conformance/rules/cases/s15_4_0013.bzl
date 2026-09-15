//! case: s15_4_0013
//! doc: spec
//! section: 15.4
//! title: a Stack<T> literal lists elements bottom-to-top
//! rule: For `Stack<T>` the first literal element is pushed first and ends up
//!   at the bottom; the last literal element is on top (the next to be popped).
//!   `[1, 2, 3]` pops 3, then 2, then 1 (§15.4).
//! cites: 15.4
//! expect: run
//! output: 3
//!   2
//!   1
module Conf;

public class Program
  public static method Main()
    using var s: Stack<int> := [1, 2, 3];
    Console.WriteLine(s.Pop());
    Console.WriteLine(s.Pop());
    Console.WriteLine(s.Pop());
  end Main;
end Program;
