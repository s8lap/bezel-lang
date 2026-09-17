//! case: s18_8_0003
//! doc: spec
//! section: 18.8
//! title: a \u escape resolving to a surrogate code point in a string literal is a compile error
//! rule: "Surrogate escapes (U+D800–U+DFFF) are compile errors." (distinct from
//!   s18_3_0002's char-literal instance of the same rule: here the escape occurs
//!   inside a string literal.)
//! cites: 18.8
//! expect: reject
//! diagnostic: surrogate
module Conf;

public class Program
  public static method Main()
    var s := "\uD800";
  end Main;
end Program;
