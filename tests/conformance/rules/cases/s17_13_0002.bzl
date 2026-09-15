//! case: s17_13_0002
//! doc: spec
//! section: 17.13
//! title: a type annotation on out _ is a syntax error
//! rule: "A type annotation on `out _` is not permitted (`out _: int` is a
//!   syntax error) ... a bare ignore takes its type from context, never an
//!   annotation."
//! cites: 17.13
//! expect: reject
//! diagnostic: annotation
module Conf;

public class Program
  public static method Main()
    var dict: Dictionary<string, int> := ["a": 1];
    var found := dict.TryGetValue("a", out _: int);
  end Main;
end Program;
