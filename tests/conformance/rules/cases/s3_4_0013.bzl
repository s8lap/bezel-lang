//! case: s3_4_0013
//! doc: spec
//! section: 3.4
//! title: String.Join rejects an int where the items sequence is declared
//! rule: public static method Join(separator: string, items: IEnumerable<string>):
//!   string;
//! cites: 3.4, 5.1, 9.6
//! expect: reject
//! diagnostic: string.Join
//! issue: 7494
//! notes: twin of s3_4_0012 (pairs the rejected with its accepting form).
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(String.Join(",", 5));
  end Main;
end Program;
