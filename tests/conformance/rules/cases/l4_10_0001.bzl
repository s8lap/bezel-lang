//! case: l4_10_0001
//! doc: stdlib
//! section: 4.10
//! title: Set<T>.TryAdd reports false on a duplicate element
//! rule: method TryAdd(item: T): bool; // returns whether added (false if
//!   duplicate).
//! cites: 4.10
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    using var s := new Set<int>();
    _ := s.TryAdd(1);
    Console.WriteLine(s.TryAdd(1));
  end Main;
end Program;
