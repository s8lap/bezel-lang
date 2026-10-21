//! case: l9_6_0008
//! doc: stdlib
//! section: 9.6
//! title: a narrowed char? search value is accepted by string.IndexOf as its char overload
//! rule: `IndexOf(value: char, startIndex: int := 0): int` takes a char search value and returns the index of the first occurrence, so `c` in `abc` is at index 2; the narrowing comes from "`x is not null` (narrowing the true branch)" ([§7.4]).
//! cites: 9.6, 7.4
//! expect: run
//! stdout: 2
//! issue: 6562
module Conf;

public class Program
  public static method Main()
    var n: char? := 'c';
    if n is not null then Console.WriteLine("abc".IndexOf(n)); end;
  end Main;
end Program;
