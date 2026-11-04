//! case: s3_4_0012
//! doc: spec
//! section: 3.4
//! title: String.Join over an Array of strings equals string.Join
//! rule: public static method Join(separator: string, items: IEnumerable<string>):
//!   string;
//! cites: 3.4, 9.6
//! expect: run
//! output: a,b,c
//!   a,b,c
//! issue: 7494
//! notes: twin of s3_4_0013 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<string>();
    xs.Add("a");
    xs.Add("b");
    xs.Add("c");
    Console.WriteLine(String.Join(",", xs));
    Console.WriteLine(string.Join(",", xs));
  end Main;
end Program;
