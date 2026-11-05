//! case: s17_7_0903
//! doc: spec
//! section: 17.7
//! title: DictionaryEntry<string, int> built with swapped argument types is rejected
//! rule: `new T(...)` resolves only against a declared constructor, never against a compiler-supplied
//!   fallback
//! cites: 4.8, 5.1
//! expect: reject
//! diagnostic: declared constructor
//! issue: 8136
module Conf;

public class Program
  public static method Main()
    var e := new DictionaryEntry<string, int>(5, "x");
    Console.WriteLine(e.Key);
  end Main;
end Program;
