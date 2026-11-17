//! case: l4_4_2519
//! doc: stdlib
//! section: 4.4
//! title: Sort() on Array<TimeSpan> and Array<Guid>: the built-in records already conform to IComparable
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 4.4, 2.8, 14.2
//! expect: run
//! output: True
//!   True
//! issue: 8584
module Conf;

public class Program
  public static method Main()
    using var a: Array<TimeSpan> := [TimeSpan.FromSeconds(30), TimeSpan.FromSeconds(10), TimeSpan.FromSeconds(20)];
    a.Sort();
    Console.WriteLine(a[0] = TimeSpan.FromSeconds(10));
    using var g: Array<Guid> := [Guid.Parse("00000000-0000-0000-0000-000000000003"), Guid.Parse("00000000-0000-0000-0000-000000000001")];
    g.Sort();
    Console.WriteLine(g[0] = Guid.Parse("00000000-0000-0000-0000-000000000001"));
  end Main;
end Program;
