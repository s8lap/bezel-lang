//! case: s28_3_6448
//! doc: spec
//! section: 28.3
//! title: a Dictionary literal returned as IEnumerable of entries is enumerated by
//!   the caller
//! rule: Collection literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`,
//!   `OwningArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc.) | Owned
//! cites: 28.3, 29.2
//! expect: run
//! stdout: 1800
module Conf;
public class Consume
  public static method F(): IEnumerable<DictionaryEntry<int, int>>
    var d: Dictionary<int, int> := [1: 1, 2: 2, 3: 3];
    return d;
  end F;
end Consume;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<300 do
      var u := Consume.F();
      for var e in u do s := s + e.Value; end;
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
