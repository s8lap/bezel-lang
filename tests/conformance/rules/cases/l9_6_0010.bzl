//! case: l9_6_0010
//! doc: stdlib
//! section: 9.6
//! title: a narrowed string? search value is accepted by string.IndexOf as its string overload
//! rule: `IndexOf(value: string, startIndex: int := 0): int` returns the index of the first occurrence at or after startIndex, so `b` in `abc` is at index 1; the narrowing comes from "`x is not null` (narrowing the true branch)" ([§7.4]).
//! cites: 9.6, 7.4
//! expect: run
//! stdout: 1
//! issue: 6562
module Conf;

public class Program
  public static method Main()
    var n: string? := "b";
    if n is not null then Console.WriteLine("abc".IndexOf(n)); end;
  end Main;
end Program;
