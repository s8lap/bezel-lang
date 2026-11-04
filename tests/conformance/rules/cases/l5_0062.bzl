//! case: l5_0062
//! doc: stdlib
//! section: 5
//! title: Guid ordering compares bytes as unsigned: 0x80 sorts above 0x7f
//! rule: CompareTo = lexicographic over the 16 canonical big-endian
//! cites: 2.6, 5
//! expect: run
//! output: 7fffffff-ffff-ffff-ffff-ffffffffffff
//!   80000000-0000-0000-0000-000000000000
//! issue: 8048
//! notes: twin of l5_0060 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    using var arr := new Array<Guid>();
    arr.Add(Guid.Parse("80000000-0000-0000-0000-000000000000"));
    arr.Add(Guid.Parse("7fffffff-ffff-ffff-ffff-ffffffffffff"));
    Console.WriteLine(arr.Min(lambda(x: Guid) => x).ToString());
    Console.WriteLine(arr.Max(lambda(x: Guid) => x).ToString());
  end Main;
end Program;
