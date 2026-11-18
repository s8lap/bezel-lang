//! case: s28_3_6446
//! doc: spec
//! section: 28.3
//! title: an Array of string literals returned as IEnumerable is enumerated by the
//!   caller
//! rule: Collection literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`,
//!   `OwningArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc.) | Owned
//! cites: 28.3, 29.2
//! expect: run
//! stdout: 1800
module Conf;
public class Consume
  public static method F(): IEnumerable<string>
    var a: Array<string> := ["a", "b", "c"];
    return a;
  end F;
end Consume;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<300 do
      var u := Consume.F();
      for var r in u do s := s + r.Length; end;
      s := s + 3;
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
