//! case: s18_3_0002
//! doc: spec
//! section: 18.3
//! title: a char escape resolving to a surrogate code point is a compile error
//! rule: "Escape sequences resolving to U+D800–U+DFFF are compile errors."
//! cites: 18.3, 18.8
//! expect: reject
//! diagnostic: surrogate
module Conf;

public class Program
  public static method Main()
    var c := '\uD800';
  end Main;
end Program;
