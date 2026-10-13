//! case: l4_16_0006
//! doc: stdlib
//! section: 4.16
//! title: a member over either heap backing declares one overload per read-only span type
//! rule: A member that accepts a read-only span of either heap backing
//!   declares two overloads, `(ReadOnlySpan<T>)` and
//!   `(ReadOnlyArraySpan<T>)`, each the direct-dispatch path; a writable
//!   span passes `.AsReadOnly()`.
//! cites: 4.16, 4.14, 4.15
//! expect: run
//! stdout: 7 8
module Conf;

public class Program
  public static method First(s: ReadOnlySpan<int>): int => s[0];
  public static method First(s: ReadOnlyArraySpan<int>): int => s[0];

  public static method Main()
    using var buf := new OwningBuffer<int>(1);
    buf[0] := 7;
    using var xs: Array<int> := [8];
    Console.WriteLine($"{First(buf.AsSpan().AsReadOnly())} {First(xs.AsReadOnlySpan())}");
  end Main;
end Program;
