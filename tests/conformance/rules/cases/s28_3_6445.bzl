//! case: s28_3_6445
//! doc: spec
//! section: 28.3
//! title: a literal Array of class elements returned as IEnumerable is enumerated by
//!   the caller
//! rule: Collection literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`,
//!   `OwningArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc.) | Owned
//! cites: 28.3, 29.2
//! expect: run
//! stdout: 1800
module Conf;
public class Box
  public V: int;
  public constructor(v: int) V := v; end constructor;
end Box;
public class Consume
  public static method F(): IEnumerable<Box>
    var a: Array<Box> := [new Box(1), new Box(2), new Box(3)];
    return a;
  end F;
end Consume;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<300 do
      var u := Consume.F();
      for var b in u do s := s + b.V; end;
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
