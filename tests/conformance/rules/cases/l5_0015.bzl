//! case: l5_0015
//! doc: stdlib
//! section: 5
//! title: GroupBy and ToDictionary keyed by a hashable type still work
//! rule: The `where TKey: hashable` member-level constraint on GroupBy and
//!   both ToDictionary overloads excludes a function type and a non-hashable
//!   record; it admits every hashable key type, which is the acceptance twin
//!   of the two rejection rows above. A gate that fired on any key selector
//!   rather than on the failing TKey would reject this program.
//! cites: 5, 14.2, 3.6 (hashable row), 27.7.1
//! expect: run
//! stdout:
//!   True
//!   True
module Conf;

public class Program
  public static method Main()
    const xs: Array<int> := [1, 2, 3];
    using var d := xs.ToDictionary(lambda(x: int): string => Program.Label(x));
    Console.WriteLine(d.Count = 3);
    var groups := xs.GroupBy(lambda(x: int): string => Program.Label(x)).Count();
    Console.WriteLine(groups = 3);
  end Main;

  public static method Label(n: int): string => $"k{n}";
end Program;