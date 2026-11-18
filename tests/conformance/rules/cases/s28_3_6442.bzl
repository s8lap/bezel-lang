//! case: s28_3_6442
//! doc: spec
//! section: 28.3
//! title: a literal-initialised Array local returned as IEnumerable feeds a lazy
//!   Where bound to a local
//! rule: Collection literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`,
//!   `OwningArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc.) | Owned
//! cites: 28.3, 29.11
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
    var a: Array<int> := [1, 2, 3];
    return a;
  end F;
end Consume;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<300 do
      using var f := Consume.F();
      using var q := f.Where(lambda(x) => x > 1);
      s := s + Consume.Total(q) + 1;
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
