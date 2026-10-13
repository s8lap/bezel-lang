//! case: s8_6_0006
//! doc: spec
//! section: 8.6
//! title: a span bound to a local declared Object is rejected at the conversion
//! rule: So a value that holds borrows (§3.6) is converted to `Object` or to an
//!   interface type at exactly one position: as the receiver of a member
//!   call ... Every other conversion — at a parameter, plain or `scoped`
//!   (§28.14), declared `Object` or any other interface type, or at a bare
//!   type parameter the call substitutes with one (`b.Keep<Object>(span)`);
//!   at a local, a field, a collection slot or collection-literal element,
//!   a record component, a choice payload, an inline-array slot, a `ref` or
//!   `out` location, or a `return`, by an implicit conversion or by an `as`
//!   or a typed pattern at such a type — is a compile error at the conversion
//! cites: 8.6, 9.7
//! expect: reject
//! diagnostic: borrow-erasing
//! issue: 5659
module Conf;

public class Client
  public static method Run(): int
    using var buf := new OwningBuffer<int>(4);
    var s := buf.AsSpan();
    var o: Object := s;
    return s.Length;
  end Run;
end Client;
