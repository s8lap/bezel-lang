//! case: s7_5_0010
//! doc: spec
//! section: 7.5
//! title: a null-conditional Count call on a null array falls back through ??
//! rule: "`x?.M` returns null if `x` is null; otherwise it performs the access." ([§7.5]) and "`a ?? b`
//!   returns `a` if non-null, else `b`." ([§7.7]): a null receiver makes the `??` take its
//!   fallback, -1.
//! cites: 7.5, 7.7
//! expect: run
//! stdout: -1
//! issue: 6529
module Conf;

public class Program
  public static method Main()
    var a: Array<int>? := null;
    Console.WriteLine(a?.Count() ?? -1);
  end Main;
end Program;
