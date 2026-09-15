//! case: l4_19_0001
//! doc: stdlib
//! section: 4.19
//! title: FrozenArray<T>.From(owned buffer) steals a value-typed buffer's storage
//! rule: static method From(owned buffer: OwningBuffer<T>): owned
//!   FrozenArray<T>; // VALUE-TYPED T ONLY: O(1) -- steals the storage; the
//!   buffer is consumed.
//! cites: 4.19
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(3, lambda(i: int): int => i);
    using var fa := FrozenArray<int>.From(move buf);
    Console.WriteLine(fa[1]);
  end Main;
end Program;
