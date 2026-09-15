//! case: l4_19_0005
//! doc: stdlib
//! section: 4.19
//! title: ToArray() returns a fresh mutable copy out of a FrozenArray<T>
//! rule: method ToArray(): owned Array<T>; // fresh mutable copy back out.
//! cites: 4.19
//! expect: run
//! stdout: 99
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(2, lambda(i: int): int => i);
    using var fa := FrozenArray<int>.From(move buf);
    using var back := fa.ToArray();
    back[0] := 99;
    Console.WriteLine(back[0]);
  end Main;
end Program;
