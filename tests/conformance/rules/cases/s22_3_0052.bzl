//! case: s22_3_0052
//! doc: spec
//! section: 22.3
//! title: a Set<int> produced by a call reaches an owned parameter with its elements intact
//! rule: "A fresh producer — a `new` expression, a call returning `owned T`, an operator result, or a `move obj._field` — stays unmarked: no live binding is invalidated, so there is nothing to announce."
//! cites: 4.10
//! expect: run
//! output: 2
module Conf;

public class Program
  public static method Build(): owned Set<int>
    var v := new Set<int>();
    v.Add(4);
    v.Add(5);
    return v;
  end Build;

  public static method Sink(owned v: Set<int>): int => v.Count;

  public static method Main()
    Console.WriteLine(Sink(Build()));
  end Main;
end Program;
