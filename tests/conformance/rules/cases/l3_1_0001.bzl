//! case: l3_1_0001
//! doc: stdlib
//! section: 3.1
//! title: Append returns this, enabling chained calls
//! rule: Methods return this to allow chaining; per-type Append overloads
//!   behave as Append(value.ToString()).
//! cites: 3.1
//! expect: run
//! stdout: ab3
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder();
    sb.Append("a").Append('b').Append(3);
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
