//! case: l5_0060
//! doc: stdlib
//! section: 5
//! title: Min and Max with a Guid-valued selector over record elements order by the canonical bytes
//! rule: CompareTo = lexicographic over the 16 canonical big-endian
//! cites: 2.6, 5
//! expect: run
//! output: 00000000-0000-0000-0000-000000000005
//!   01000000-0000-0000-0000-000000000000
//! issue: 8048
//! notes: twin of l5_0061 (pairs the accepted with its rejecting form).
module Conf;

public record Item(Id: Guid, N: int);

public class Program
  public static method Main()
    using var arr := new Array<Item>();
    arr.Add(new Item(Guid.Parse("00000000-0000-0000-0001-000000000000"), 1));
    arr.Add(new Item(Guid.Parse("01000000-0000-0000-0000-000000000000"), 2));
    arr.Add(new Item(Guid.Parse("00000000-0000-0000-0000-000000000005"), 3));
    Console.WriteLine(arr.Min(lambda(x: Item) => x.Id).ToString());
    Console.WriteLine(arr.Max(lambda(x: Item) => x.Id).ToString());
  end Main;
end Program;
