//! case: l9_6_0055
//! doc: stdlib
//! section: 9.6
//! title: string IndexOf given an int as its named value argument is rejected
//! rule: public pure method IndexOf(value: char, startIndex: int := 0): int; ... public pure method IndexOf(value: string, startIndex: int := 0): int;
//! cites: 9.6
//! expect: reject
//! diagnostic: takes its search value at position 0
module Conf;

public class Program
  public static method Find(s: string): int
    return s.IndexOf(startIndex := 2, value := 5);
  end Find;
end Program;
