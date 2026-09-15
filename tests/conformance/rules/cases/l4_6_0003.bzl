//! case: l4_6_0003
//! doc: stdlib
//! section: 4.6
//! title: a sequence literal constructs a LinkedList<T> in head-to-tail order
//! rule: A LinkedList<T> may be constructed with a sequence literal;
//!   textual order is head-to-tail: `using var l: LinkedList<int> :=
//!   [10, 20, 30];` produces a list whose First is 10 and Last is 30.
//! cites: 4.6, 15.4
//! expect: run
//! stdout: 10
module Conf;

public class Program
  public static method Main()
    using var l: LinkedList<int> := [10, 20, 30];
    Console.WriteLine(l.First);
  end Main;
end Program;
