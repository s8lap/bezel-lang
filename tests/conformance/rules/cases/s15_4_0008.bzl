//! case: s15_4_0008
//! doc: spec
//! section: 15.4
//! title: a Set<T> literal collapses duplicates, retaining the first occurrence
//! rule: For `Set<T>`, duplicate elements are silently collapsed; the first
//!   occurrence in textual order is retained (§15.4).
//! cites: 15.4
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    using var s: Set<int> := [1, 2, 2, 3];
    Console.WriteLine(s.Count);
  end Main;
end Program;
