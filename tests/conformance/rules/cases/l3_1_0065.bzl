//! case: l3_1_0065
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Insert with a string index is rejected
//! rule: public method Insert(index: int, value: string): StringBuilder;
//! cites: 3.1, 22.6
//! expect: reject
//! diagnostic: index
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("abc");
    sb.Insert("x", "y");
  end Main;
end Program;
