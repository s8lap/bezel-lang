//! case: s7_1_0901
//! doc: spec
//! section: 7.1
//! title: a nullable string is not passed as the non-nullable Key of a DictionaryEntry
//! rule: A non-nullable reference type implicitly converts to its nullable form. The reverse requires a
//!   null check or `!`.
//! cites: 4.8, 17.7
//! expect: reject
//! diagnostic: null check
//! issue: 8136
module Conf;

public class Program
  public static method Main()
    var s: string? := null;
    var e := new DictionaryEntry<string, int>(s, 1);
    Console.WriteLine(e.Key);
  end Main;
end Program;
