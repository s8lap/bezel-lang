//! case: s28_3_6444
//! doc: spec
//! section: 28.3
//! title: an Array local borrowed by a call and then returned as IEnumerable is
//!   still produced owned
//! rule: Collection literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`,
//!   `OwningArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc.) | Owned
//! cites: 28.3, 28.3.4
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
    var t := Consume.Total(a);
    if t < 0 then return a; end;
    return a;
  end F;
end Consume;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<300 do
      var u := Consume.F();
      s := s + Consume.Total(u);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
