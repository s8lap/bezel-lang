//! case: s5_1_0012
//! doc: spec
//! section: 5.1
//! title: an integer does not implicitly convert to bool
//! rule: The following are never implicit: between integers and bool.
//! cites: 5.1
//! expect: reject
//! diagnostic: never implicit
module Conf;

public class Program
  public static method Convert(n: int)
    var b: bool := n;
  end Convert;
end Program;
