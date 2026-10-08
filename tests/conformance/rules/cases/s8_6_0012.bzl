//! case: s8_6_0012
//! doc: spec
//! section: 8.6
//! title: a byte span boxes as the receiver of the IReadOnlySpan<byte> read-tier extension
//! rule: So a value that holds borrows (§3.6) is converted to `Object` or to an
//!   interface type at exactly two positions: as the receiver of a member
//!   call ... or of an extension-block call whose head is
//!   `IReadOnlySpan<TElement>` (§26.2, §26.5 rule 2), and at a position
//!   declared `IReadOnlySpan<TElement>` (stdlib §4.16) ... Every other
//!   conversion — at a parameter, plain or `scoped` (§28.14), declared
//!   `Object` or any other interface type, or at a bare type parameter the
//!   call substitutes with one (`b.Keep<Object>(span)`); at a local, a field,
//!   a collection slot or collection-literal element, a record component, a
//!   choice payload, an inline-array slot, a `ref` or `out` location, or a
//!   `return`, by an implicit conversion or by an `as` or a typed pattern at
//!   such a type — is a compile error at the conversion
//! cites: 8.6, 9.7, 26.5
//! expect: run
//! stdout: 7
module Conf;

public class Program
  public static method Main()
    using var buf := new OwningBuffer<byte>(4);
    buf[0] := byte(7);
    var s := buf.AsSpan();
    Console.WriteLine(s.ReadInt32LittleEndian());
  end Main;
end Program;
