//! case: l4_19_0004
//! doc: stdlib
//! section: 4.19
//! title: freezing a buffer with a live span local over it is rejected by the consume-site freeze
//! rule: A live local of borrow-bearing record type counts as a live local
//!   borrow of every held borrow's source: var s := buf.AsSpan(); freezes buf
//!   against FrozenArray<T>.From(move buf) -- and every other consume -- for
//!   as long as s is live, a borrow being live at a consume site when some
//!   path from the site reaches a later use of it.
//! cites: 4.19, 28.3, 9.7
//! expect: reject
//! diagnostic: live
module Conf;

public class Program
  public static method Main()
    var buf := new OwningBuffer<int>(3);
    var s := buf.AsSpan();
    using var fa := FrozenArray<int>.From(move buf);
    Console.WriteLine(s.Length);
  end Main;
end Program;
