//! case: l5_0017
//! doc: stdlib
//! section: 5
//! title: GroupBy merges a string key built at run time with an equal literal key
//! rule: "Groups appear in first-occurrence-of-key order; elements within a group in source order." (stdlib [§5]); a string is compared by content ("A reference type with content-based equality." [§4.6]), so the concatenated key `ax` and the literal `ax` fall in one group.
//! cites: 5, 4.6
//! expect: run
//! output: ax=2
//!   y=1
//! issue: 6532
module Conf;

public class Program
  public static method Main()
    var p := "a";
    var xs: Array<string> := [p + "x", "y", "ax"];
    for var g in xs.GroupBy(lambda(s: string) => s) do
      Console.WriteLine(g.Key + "=" + g.Count().ToString());
    end;
  end Main;
end Program;
