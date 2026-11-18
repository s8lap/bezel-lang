//! case: s7_5_7107
//! doc: spec
//! section: 7.5
//! title: `?.First` on a non-null empty SortedSet raises; a null receiver gives null
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 7.7
//! expect: run
//! output: -99
//!   -1
//!   5
module Conf;
public class Program
  public static method Read(s: SortedSet<int>?): int
    try
      return s?.First ?? -1;
    catch InvalidOperationException =>
      return -99;
    end;
  end Read;
  public static method Main()
    using var e := new SortedSet<int>();
    using var one := new SortedSet<int>();
    one.Add(5);
    Console.WriteLine(Read(e));
    Console.WriteLine(Read(null));
    Console.WriteLine(Read(one));
  end Main;
end Program;
