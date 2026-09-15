//! case: s18_5_0012
//! doc: spec
//! section: 18.5
//! title: an indexer inside a hole needs no parenthesization before a format spec
//! rule: "the alignment `,` and the format-spec `:` are recognized only at the
//!   top nesting level of the hole ... Consequently `{d[k]:F2}` needs no
//!   parenthesization tricks (`[…]` nests its colon)."
//! cites: 18.5
//! expect: run
//! stdout: 3.14
module Conf;

public class Program
  public static method Main()
    var d: Dictionary<string, double> := ["x": 3.14159];
    var k := "x";
    Console.WriteLine($"{d[k]:F2}");
  end Main;
end Program;
