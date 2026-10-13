//! case: l4_14_0017
//! doc: stdlib
//! section: 4.14
//! title: a buffer span's SequenceEqual does not take a writable array span
//! rule: Every span type declares it for its own type and for the read-only
//!   span of each heap backing — here `Span<T>`, `ReadOnlySpan<T>` and
//!   `ReadOnlyArraySpan<T>` ... — so a comparison across backings needs no
//!   common type, and a writable span passed as the argument converts by
//!   `.AsReadOnly()`.
//! cites: 4.14, 4.15
//! expect: reject
//! diagnostic: SequenceEqual
//! issue: 5921
module Conf;

public class Program
  public static method Same(): bool
    using var buf := new OwningBuffer<int>(2);
    using var xs: Array<int> := [0, 0];
    var s := buf.AsSpan();
    return s.SequenceEqual(xs.AsSpan());
  end Same;
end Program;
