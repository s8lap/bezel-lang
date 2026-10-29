//! case: l3_1_0064
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Insert with one argument is rejected
//! rule: public method Insert(index: int, value: string): StringBuilder;
//! cites: 3.1, 22.6
//! expect: reject
//! diagnostic: Insert
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("abc");
    sb.Insert(0);
  end Main;
end Program;
