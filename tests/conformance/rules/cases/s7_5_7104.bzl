//! case: s7_5_7104
//! doc: spec
//! section: 7.5
//! title: `?.First` on a LinkedList<int?> is `int?` whether the receiver, or the stored element, is null
//! rule: The result type is always nullable
//! cites: 7.5, 7.7
//! expect: run
//! output: 99
//!   3
module Conf;
public class Program
  public static method Main()
    using var li := new LinkedList<int?>();
    _ := li.AddLast(null);
    _ := li.AddLast(3);
    Console.WriteLine(li?.First ?? 99);
    Console.WriteLine(li?.Last ?? 99);
  end Main;
end Program;
