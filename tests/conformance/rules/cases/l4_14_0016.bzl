//! case: l4_14_0016
//! doc: stdlib
//! section: 4.14
//! title: a buffer span compares with a writable array span passed through AsReadOnly
//! rule: Every span type declares it for its own type and for the read-only
//!   span of each heap backing — here `Span<T>`, `ReadOnlySpan<T>` and
//!   `ReadOnlyArraySpan<T>` ... — so a comparison across backings needs no
//!   common type, and a writable span passed as the argument converts by
//!   `.AsReadOnly()`.
//! cites: 4.14, 4.15
//! expect: run
//! output: True
//!   False
//! issue: 5921
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<int>(2);
    buf[0] := 1;
    buf[1] := 2;
    using var same: Array<int> := [1, 2];
    using var other: Array<int> := [1, 3];
    var s := buf.AsSpan();
    Console.WriteLine(s.SequenceEqual(same.AsSpan().AsReadOnly()));
    Console.WriteLine(s.SequenceEqual(other.AsSpan().AsReadOnly()));
  end Main;
end Program;
