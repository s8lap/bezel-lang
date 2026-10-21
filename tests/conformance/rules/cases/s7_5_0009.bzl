//! case: s7_5_0009
//! doc: spec
//! section: 7.5
//! title: a null-conditional Count call on a non-null array yields its count
//! rule: "`x?.M` returns null if `x` is null; otherwise it performs the access." ([§7.5]). With a
//!   non-null receiver the access is the call `Count()`, which yields the element count, 2.
//! cites: 7.5, 7.7
//! expect: run
//! stdout: 2
//! issue: 6529
module Conf;

public class Program
  public static method Main()
    var a: Array<int>? := [4, 2];
    Console.WriteLine(a?.Count() ?? -1);
  end Main;
end Program;
