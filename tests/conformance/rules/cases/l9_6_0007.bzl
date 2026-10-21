//! case: l9_6_0007
//! doc: stdlib
//! section: 9.6
//! title: a narrowed int? search value is rejected by string.IndexOf, which takes a char or a string
//! rule: `IndexOf(value: char, startIndex: int := 0): int` and `IndexOf(value: string, startIndex: int := 0): int` take a char or a string search value; after `x is not null` (narrowing the true branch) the value is an int, and "a conversion §5.1 does not admit as implicit" leaves no candidate reachable ([§22.6]).
//! cites: 9.6, 7.4, 22.6, 5.1
//! expect: reject
//! diagnostic: IndexOf
//! issue: 6562
module Conf;

public class Program
  public static method Main()
    var n: int? := 1;
    if n is not null then Console.WriteLine("abc".IndexOf(n)); end;
  end Main;
end Program;
