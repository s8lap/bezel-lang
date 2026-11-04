//! case: l4_10_0039
//! doc: stdlib
//! section: 4.10
//! title: a Set takes its capacity as an int at the position that also carries a comparer
//! rule: public constructor(capacity: int);
//! cites: 4.10, 17.7
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    using var s := new Set<int>(16);
    s.Add(1);
    s.Add(1);
    s.Add(2);
    Console.WriteLine(s.Count);
  end Main;
end Program;
