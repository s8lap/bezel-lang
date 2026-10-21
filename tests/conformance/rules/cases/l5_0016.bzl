//! case: l5_0016
//! doc: stdlib
//! section: 5
//! title: GroupBy on a string key puts each distinct key in one group, counting its equal elements together
//! rule: "Groups appear in first-occurrence-of-key order; elements within a group in source order." (stdlib [§5]); one group per distinct key, the string key compared by content ("A reference type with content-based equality." [§4.6]), so the two `aa` elements share one group.
//! cites: 5, 4.6
//! expect: run
//! output: aa=2
//!   bb=1
//! issue: 6532
module Conf;

public class Program
  public static method Main()
    const xs: Array<string> := ["aa", "bb", "aa"];
    for var g in xs.GroupBy(lambda(s: string) => s) do
      Console.WriteLine(g.Key + "=" + g.Count().ToString());
    end;
  end Main;
end Program;
