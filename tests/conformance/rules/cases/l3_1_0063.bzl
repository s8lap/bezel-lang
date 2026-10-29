//! case: l3_1_0063
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Remove with a string startIndex is rejected
//! rule: public method Remove(startIndex: int, length: int): StringBuilder;
//! cites: 3.1, 22.6
//! expect: reject
//! diagnostic: startIndex
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("abc");
    sb.Remove("a", 1);
  end Main;
end Program;
