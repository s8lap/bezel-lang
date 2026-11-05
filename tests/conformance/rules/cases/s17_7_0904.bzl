//! case: s17_7_0904
//! doc: spec
//! section: 17.7
//! title: DictionaryEntry built with a named argument that names no parameter is rejected
//! rule: `new T(...)` resolves only against a declared constructor, never against a compiler-supplied
//!   fallback
//! cites: 4.8, 22.5
//! expect: reject
//! diagnostic: declared constructor
//! issue: 8136
module Conf;

public class Program
  public static method Main()
    var e := new DictionaryEntry<string, int>(Key := "a", Value := 1, Foo := 2);
    Console.WriteLine(e.Key);
  end Main;
end Program;
