//! case: l4_11_0004
//! doc: stdlib
//! section: 4.11
//! title: a Stack<T> literal is read bottom-to-top, so the last element pops first
//! rule: Stack<T>/OwningStack<T>: the literal lists elements bottom-to-top
//!   — the first element is pushed first (ends up at the bottom), the
//!   last element is on top (popped first). [1, 2, 3] pops 3, then 2,
//!   then 1.
//! cites: 4.11, 15.4
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
