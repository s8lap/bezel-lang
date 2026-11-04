//! case: s3_4_0009
//! doc: spec
//! section: 3.4
//! title: String.Repeat rejects an int where the unit string is declared
//! rule: public static pure method Repeat(unit: string, count: int): string;
//! cites: 3.4, 5.1, 9.6
//! expect: reject
//! diagnostic: string.Repeat
//! issue: 7494
//! notes: twin of s3_4_0008 (pairs the rejected with its accepting form).
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(String.Repeat(5, 2));
  end Main;
end Program;
