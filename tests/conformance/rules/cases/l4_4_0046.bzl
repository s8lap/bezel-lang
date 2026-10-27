//! case: l4_4_0046
//! doc: stdlib
//! section: 4.4
//! title: IndexOf's defaulted startIndex is 0 and the first match is found
//! rule: `public method IndexOf(item: T, startIndex: int := 0): int;` (stdlib §4.4,
//!   v0.340) — the first occurrence at or after `startIndex`, or -1. The call omits the
//!   optional `startIndex`, so the default 0 applies and the search starts at the first
//!   slot: the elements 1, 2, 3 hold 2 at index 1.
//! cites: 4.4, 22.5
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(a.IndexOf(2));
  end Main;
end Program;
