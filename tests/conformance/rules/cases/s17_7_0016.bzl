//! case: s17_7_0016
//! doc: spec
//! section: 17.7
//! title: a string cannot stand for the capacity of a Dictionary: no constructor takes it
//! rule: `new T(...)` resolves only against a declared constructor, never against a
//!   compiler-supplied fallback
//! cites: 17.7, 5.1, 4.8
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<int, string>("x");
  end Main;
end Program;
