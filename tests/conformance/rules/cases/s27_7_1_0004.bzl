//! case: s27_7_1_0004
//! doc: spec
//! section: 27.7.1
//! title: a collection of callbacks is an Array whose Contains compares by function-value equality
//! rule: a `Set<(int) -> int>`, a `Dictionary<(int) -> int, V>`, stdlib §5's
//!   hashable block and `where T: hashable` at a function type are rejected
//!   at the instantiation site, and a collection of callbacks is an
//!   `Array<(int) -> int>`, whose `Contains` and `Remove` compare by this
//!   equality
//! cites: 27.7.1, 3.6
//! expect: run
//! stdout: True
module Conf;

public class Demo
  public static method Twice(x: int): int => x * 2;

  public static method Main()
    var fs := new Array<(int) -> int>();
    fs.Add(Demo.Twice);
    Console.WriteLine(fs.Contains(Demo.Twice));
  end Main;
end Demo;
