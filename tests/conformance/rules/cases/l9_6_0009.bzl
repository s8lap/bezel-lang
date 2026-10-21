//! case: l9_6_0009
//! doc: stdlib
//! section: 9.6
//! title: a narrowed string? receiver answers IndexOf with the char argument's first occurrence
//! rule: `IndexOf(value: char, startIndex: int := 0): int` returns the code-point index of the first occurrence, so `c` in `abc` is at index 2; the receiver is narrowed by "`x is not null` (narrowing the true branch)" ([§7.4]) before its member is used.
//! cites: 9.6, 7.4
//! expect: run
//! stdout: 2
//! issue: 6562
module Conf;

public class Program
  public static method Main()
    var s: string? := "abc";
    if s is not null then Console.WriteLine(s.IndexOf("c")); end;
  end Main;
end Program;
