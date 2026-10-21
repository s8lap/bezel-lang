//! case: s7_5_0013
//! doc: spec
//! section: 7.5
//! title: a local named Count does not change a null-conditional Count call
//! rule: "`x?.M` returns null if `x` is null; otherwise it performs the access." ([§7.5]) "`a ?? b`
//!   returns `a` if non-null, else `b`." ([§7.7]) A local named `Count` is a different binding from
//!   the member access `a?.Count()`, so `Count ?? -9` is the local's value, -9.
//! cites: 7.5, 7.7
//! expect: run
//! output: 2
//!   -9
//! issue: 6681
module Conf;

public class Program
  public static method Main()
    var Count: int? := null;
    var a: Array<int>? := [4, 2];
    Console.WriteLine(a?.Count() ?? -1);
    Console.WriteLine(Count ?? -9);
  end Main;
end Program;
