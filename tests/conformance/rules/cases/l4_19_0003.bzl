//! case: l4_19_0003
//! doc: stdlib
//! section: 4.19
//! title: FrozenArray<T> has no indexer setter -- deeply immutable after construction
//! rule: public property [i: int]: T get; end; -- FrozenArray<T> declares no
//!   mutating member, so no writer can exist after construction.
//! cites: 4.19
//! expect: reject
//! diagnostic: readonly
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(1);
    using var fa := FrozenArray<int>.From(move buf);
    fa[0] := 9;
  end Main;
end Program;
