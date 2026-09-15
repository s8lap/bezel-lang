//! case: l4_18_0003
//! doc: stdlib
//! section: 4.18
//! title: TryAdd on an already-present element returns false
//! rule: method TryAdd(item: T): bool; // false if an equal item is present.
//! cites: 4.18
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    using var ss := new SortedSet<int>();
    ss.Add(1);
    Console.WriteLine(ss.TryAdd(1));
  end Main;
end Program;
