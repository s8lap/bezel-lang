//! case: s7_5_0028
//! doc: spec
//! section: 7.5
//! title: the result of ?. over a string member is nullable and does not bind to an int
//! rule: The result type is always nullable
//! cites: 7.5
//! expect: reject
//! diagnostic: nullable
//! issue: 7747
module Conf;

public class Program
  public static method Main()
    var s: string? := "abcde";
    var a: int := s?.Length;
  end Main;
end Program;
