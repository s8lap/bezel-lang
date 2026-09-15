//! case: s5_2_0007
//! doc: spec
//! section: 5.2
//! title: an explicit conversion from bool to double is rejected
//! rule: A bool source likewise does not convert to float/double/decimal (no
//!   blessed numeric reading) or char (a code point, not a number).
//! cites: 5.2, 4.1
//! expect: reject
//! diagnostic: numeric reading
module Conf;

public class Program
  public static method Main()
    var flag: bool := true;
    var d := double(flag);
    Console.WriteLine(d);
  end Main;
end Program;
