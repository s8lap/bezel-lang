//! case: l5_0018
//! doc: stdlib
//! section: 5
//! title: GroupBy keeps a string key and a longer string that extends it in separate groups
//! rule: "Groups appear in first-occurrence-of-key order; elements within a group in source order." (stdlib [§5]); a string key is compared by its whole content ("A reference type with content-based equality." [§4.6]), so `ab` and `abc` are distinct keys and never share a group.
//! cites: 5, 4.6
//! expect: run
//! output: ab=2
//!   abc=2
//! issue: 6532
module Conf;

public class Program
  public static method Main()
    const xs: Array<string> := ["ab", "abc", "ab", "abc"];
    for var g in xs.GroupBy(lambda(s: string) => s) do
      Console.WriteLine(g.Key + "=" + g.Count().ToString());
    end;
  end Main;
end Program;
