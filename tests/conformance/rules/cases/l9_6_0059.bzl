//! case: l9_6_0059
//! doc: stdlib
//! section: 9.6
//! title: string PadLeft given an int as its named paddingChar argument is rejected
//! rule: public pure method PadLeft(totalWidth: int, paddingChar: char := ' '): string;
//! cites: 9.6
//! expect: reject
//! diagnostic: argument to `string.PadLeft` has a type that is not implicitly convertible
module Conf;

public class Program
  public static method Pad(s: string): string
    return s.PadLeft(paddingChar := 20, totalWidth := '*');
  end Pad;
end Program;
