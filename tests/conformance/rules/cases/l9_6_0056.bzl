//! case: l9_6_0056
//! doc: stdlib
//! section: 9.6
//! title: string Insert given an int as its named value argument is rejected
//! rule: public pure method Insert(startIndex: int, value: string): string;
//! cites: 9.6
//! expect: reject
//! diagnostic: argument to `string.Insert` has a type that is not implicitly convertible
module Conf;

public class Program
  public static method Put(s: string): string
    return s.Insert(startIndex := 3, value := 5);
  end Put;
end Program;
