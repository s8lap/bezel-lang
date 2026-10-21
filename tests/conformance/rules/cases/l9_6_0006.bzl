//! case: l9_6_0006
//! doc: stdlib
//! section: 9.6
//! title: an int search value is rejected by string.IndexOf, which takes a char or a string
//! rule: `IndexOf(value: char, startIndex: int := 0): int` and `IndexOf(value: string, startIndex: int := 0): int` take a char or a string search value; "a conversion §5.1 does not admit as implicit" leaves no candidate reachable ([§22.6]), so the call is a compile error.
//! cites: 9.6, 22.6, 5.1
//! expect: reject
//! diagnostic: IndexOf
//! issue: 6562
module Conf;

public class Program
  public static method Run(n: int): int
    return "abc".IndexOf(n);
  end Run;

  public static method Main()
    Console.WriteLine(Run(1));
  end Main;
end Program;
