//! case: s3_4_0015
//! doc: spec
//! section: 3.4
//! title: String.From rejects an int argument
//! rule: public static pure method From(value: char): string;
//! cites: 3.4, 5.1, 9.6
//! expect: reject
//! diagnostic: String.From
//! issue: 7494
//! notes: twin of s3_4_0014 (pairs the rejected with its accepting form).
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(String.From(5));
  end Main;
end Program;
