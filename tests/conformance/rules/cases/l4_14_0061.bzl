//! case: l4_14_0061
//! doc: stdlib
//! section: 4.14
//! title: an OwningBuffer's Length reads its stored extent
//! rule: `public pure readonly property Length: int;    // stored extent, immutable once` (stdlib §4.14, OwningBuffer<T>, no IEnumerable conformance).
//!   A buffer constructed with length 4 has a stored extent of 4.
//! cites: 4.14
//! expect: run
//! stdout: 4
//! issue: 6081
module Conf;

public class C
  public static method Main(): int
    using var buf := new OwningBuffer<byte>(4);
    Console.WriteLine(buf.Length);
    return 0;
  end Main;
end C;
