//! case: s28_3_6447
//! doc: spec
//! section: 28.3
//! title: an empty collection literal bound to an Array local and returned as
//!   IEnumerable yields nothing
//! rule: Collection literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`,
//!   `OwningArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc.) | Owned
//! cites: 28.3, 29.2
//! expect: run
//! stdout: 1800
module Conf;
public class Consume
  public static method Total(e: IEnumerable<int>): int
    var s := 0;
    for var x in e do s := s + x; end;
    return s;
  end Total;
  public static method F(): IEnumerable<int>
    var a: Array<int> := [];
    return a;
  end F;
end Consume;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<300 do
      var u := Consume.F();
      s := s + Consume.Total(u) + 6;
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
