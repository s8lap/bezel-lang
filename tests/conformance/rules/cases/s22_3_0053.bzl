//! case: s22_3_0053
//! doc: spec
//! section: 22.3
//! title: a named Set<int> binding fed with the move marker reaches an owned parameter
//! rule: "the caller writes the transfer marker at the call site: `M(move x)`".
//! cites: 4.10
//! expect: run
//! output: 2
module Conf;

public class Program
  public static method Sink(owned v: Set<int>): int => v.Count;

  public static method Main()
    var s := new Set<int>();
    s.Add(4);
    s.Add(5);
    Console.WriteLine(Sink(move s));
  end Main;
end Program;
