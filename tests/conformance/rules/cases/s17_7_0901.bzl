//! case: s17_7_0901
//! doc: spec
//! section: 17.7
//! title: a one-argument construction of DictionaryEntry is rejected: its only constructor takes Key and
//!   Value
//! rule: `new T(...)` resolves only against a declared constructor, never against a compiler-supplied
//!   fallback
//! cites: 4.8, 9.1
//! expect: reject
//! diagnostic: declared constructor
//! issue: 8136
module Conf;

public class Program
  public static method Main()
    var e := new DictionaryEntry<string, int>("a");
    Console.WriteLine(e.Key);
  end Main;
end Program;
