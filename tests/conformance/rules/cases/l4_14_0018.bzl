//! case: l4_14_0018
//! doc: stdlib
//! section: 4.14
//! title: at a class T a buffer span compares with a non-nullable array span and a null slot equals nothing
//! rule: The overloads are declared at the receiver's own `T`: at a class `T`
//!   the buffer span's `T?` slots compare with an array span's `T` slots,
//!   `Equals` null-safe on the nullable side, so a null slot equals no
//!   element.
//! cites: 4.14, 4.15, 8.3
//! expect: run
//! output: True
//!   False
//! issue: 5921
module Conf;

public class W
  public readonly property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  public method Equals(other: W): bool => N = other.N;
  public override method GetHashCode(): int => N;
end W;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<W>(2);
    buf[0] := new W(1);
    buf[1] := new W(2);
    using var a := new W(1);
    using var b := new W(2);
    using var xs: Array<W> := [a, b];
    Console.WriteLine(buf.AsReadOnlySpan().SequenceEqual(xs.AsReadOnlySpan()));
    buf[1] := null;
    Console.WriteLine(buf.AsReadOnlySpan().SequenceEqual(xs.AsReadOnlySpan()));
  end Main;
end Program;
