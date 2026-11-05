//! case: s7_5_0907
//! doc: spec
//! section: 7.5
//! title: a LINQ lambda after ?. on a nullable Array field sees the element type; a null receiver gives null
//! rule: a suffix to the right of a `?.` is typed at the value it meets on the path where it runs
//!   — the preceding segment's own declared type, never the nullable type the `?.` gives the
//!   chain's result
//! cites: 27.4
//! expect: run
//! output: 3
//!   2
//!   -1
//! issue: 8166
module Conf;

public class Holder
  public owned Items: Array<string>?;

  public constructor()
  end constructor;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    h.Items := "a,b,cc".Split(",");
    var r := h.Items?.Select(lambda(x) => x.Length)?.ToArray();
    Console.WriteLine(r!.Count);
    Console.WriteLine(r![2]);
    h.Items := null;
    Console.WriteLine(h.Items?.Select(lambda(x) => x.Length)?.ToArray()?.Count ?? -1);
  end Main;
end Program;
