//! case: l9_6_0057
//! doc: stdlib
//! section: 9.6
//! title: string Replace given an int as its named newChar argument is rejected
//! rule: public pure method Replace(oldChar: char, newChar: char): string; ... public pure method Replace(oldValue: string, newValue: string): string;
//! cites: 9.6
//! expect: reject
//! diagnostic: `string.Replace` takes its replacement value at position 1
module Conf;

public class Program
  public static method Swap(s: string): string
    return s.Replace(newChar := 5, oldChar := 'l');
  end Swap;
end Program;
