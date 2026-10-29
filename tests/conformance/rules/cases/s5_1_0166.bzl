//! case: s5_1_0166
//! doc: spec
//! section: 5.1
//! title: a bool argument at the StringBuilder constructor is rejected
//! rule: Between integers and `bool`.
//! cites: 5.1
//! expect: reject
//! diagnostic: not implicitly convertible to the target type
module Conf;

public class Program
  public static method Main()
    var v := true;
    var sb := new StringBuilder(v);
    Console.WriteLine(sb.Length);
  end Main;
end Program;
