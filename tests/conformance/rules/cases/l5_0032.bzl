//! case: l5_0032
//! doc: stdlib
//! section: 5
//! title: null-conditional First, Sum, Any and ToArray on nullable list interfaces take the null branch
//! rule: ?. short-circuits on a null receiver (spec 7.5); on non-null receivers the section 5 operators are reached through IList<int>? and IReadOnlyList<int>?.
//! cites: 7.5, 5
//! expect: run
//! output: 10
//!   24
//!   True
//!   False
//!   True
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    var x: IList<int>? := a;
    var n: IList<int>? := null;
    var ro: IReadOnlyList<int>? := a;
    var rn: IReadOnlyList<int>? := null;
    Console.WriteLine((x?.First() ?? 0) + (n?.First() ?? 5));
    Console.WriteLine((ro?.Sum() ?? 0) + (rn?.Sum() ?? 7));
    Console.WriteLine(x?.Any() ?? false);
    Console.WriteLine(n?.Any() ?? false);
    var u := rn?.ToArray();
    Console.WriteLine(u = null);
  end Main;
end Program;
