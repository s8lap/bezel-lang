//! case: l3_1_0062
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Replace with integer arguments is rejected
//! rule: public method Replace(oldValue: string, newValue: string): StringBuilder;
//! cites: 3.1, 22.6
//! expect: reject
//! diagnostic: oldValue
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder("abc");
    sb.Replace(1, 2);
  end Main;
end Program;
