//! case: l4_4_2501
//! doc: stdlib
//! section: 4.4
//! title: an untyped comparer lambda over a record array's string member sorts like the typed one
//! rule: public method Sort(comparer: (T, T) -> int);  // in place
//! cites: 27.4, 14.5
//! expect: run
//! output: True True
//! issue: 8387
module Conf;
record W(K: string, V: int);

public class Program
  public static method Main()
    var typed: Array<W> := [new W("b", 1), new W("c", 2), new W("a", 3)];
    var untyped: Array<W> := [new W("b", 1), new W("c", 2), new W("a", 3)];
    typed.Sort(lambda(x: W, y: W): int => x.K.CompareTo(y.K));
    untyped.Sort(lambda(x, y) => x.K.CompareTo(y.K));
    var same := true;
    for var i in 0..<3 do
      if typed[i].K <> untyped[i].K then
        same := false;
      end if;
    end for;
    var moved := typed[0].K <> "b";
    Console.WriteLine($"{same} {moved}");
  end Main;
end Program;
