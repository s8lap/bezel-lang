//! case: l9_6_0058
//! doc: stdlib
//! section: 9.6
//! title: string Replace given an int as its named oldValue argument is rejected
//! rule: public pure method Replace(oldChar: char, newChar: char): string; ... public pure method Replace(oldValue: string, newValue: string): string;
//! cites: 9.6
//! expect: reject
//! diagnostic: `string.Replace` takes its search value at position 0
module Conf;

public class Program
  public static method Swap(s: string): string
    return s.Replace(newValue := "x", oldValue := 5);
  end Swap;
end Program;
