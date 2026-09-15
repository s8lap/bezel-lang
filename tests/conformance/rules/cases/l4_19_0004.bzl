//! case: l4_19_0004
//! doc: stdlib
//! section: 4.19
//! title: freezing a buffer with a live span local over it is rejected by the consume-site freeze
//! rule: var s := buf.AsSpan(); freezes buf against
//!   FrozenArray<T>.From(move buf) -- and every other consume -- for as
//!   long as s is live.
//! cites: 4.19, 28.3
//! expect: reject
//! diagnostic: live
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(3);
    var s := buf.AsSpan();
    using var fa := FrozenArray<int>.From(move buf);
  end Main;
end Program;
