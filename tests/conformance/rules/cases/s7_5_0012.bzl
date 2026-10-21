//! case: s7_5_0012
//! doc: spec
//! section: 7.5
//! title: the nullable result of a null-conditional Count call is well-typed with ?? to int
//! rule: "The result type is always nullable" ([§7.5]); "`a ?? b` returns `a` if non-null, else `b`."
//!   ([§7.7]): with the fallback `0` the `int` binding is well-typed and holds the count, 2.
//! cites: 7.5, 7.7
//! expect: run
//! stdout: 2
//! issue: 6529
module Conf;

public class Program
  public static method Main()
    var a: Array<int>? := [4, 2];
    var n: int := a?.Count() ?? 0;
    Console.WriteLine(n);
  end Main;
end Program;
