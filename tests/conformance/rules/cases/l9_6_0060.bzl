//! case: l9_6_0060
//! doc: stdlib
//! section: 9.6
//! title: string CompareTo given a string as its named ignoreCase argument is rejected
//! rule: public pure method CompareTo(other: string, ignoreCase: bool): int;
//! cites: 9.6
//! expect: reject
//! diagnostic: no matching overload of `string.CompareTo`
module Conf;

public class Program
  public static method Cmp(s: string): int
    return s.CompareTo(ignoreCase := "x", other := "y");
  end Cmp;
end Program;
