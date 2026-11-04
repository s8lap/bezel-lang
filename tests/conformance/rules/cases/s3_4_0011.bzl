//! case: s3_4_0011
//! doc: spec
//! section: 3.4
//! title: String.IsNullOrWhiteSpace rejects an int argument
//! rule: public static pure method IsNullOrWhiteSpace(value: string?): bool;
//! cites: 3.4, 5.1, 9.6
//! expect: reject
//! diagnostic: string.IsNullOrWhiteSpace
//! issue: 7494
//! notes: twin of s3_4_0010 (pairs the rejected with its accepting form).
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(String.IsNullOrWhiteSpace(5));
  end Main;
end Program;
