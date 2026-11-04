//! case: s4_6_0801
//! doc: spec
//! section: 4.6
//! title: Source of a span is a string, so adding an int to it is rejected
//! rule: The compiler does not implicitly convert other types to `string`; use interpolation or `.ToString()`.
//! cites: 4.6
//! expect: reject
//! diagnostic: string
//! issue: 8135
//! notes: Source has type string (stdlib 3.3); s + t concatenates two strings only (spec 4.6). Twin: the same span read in l3_3_0801.
module Conf;

public class Program
  public static method Main()
    var s := new ReadOnlyStringSpan("hello", 1, 3);
    var t := s.Source + 1;
  end Main;
end Program;
