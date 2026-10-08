//! case: s8_6_0005
//! doc: spec
//! section: 8.6
//! title: a span passed to a parameter declared Object is rejected at the conversion
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
//! cites: 8.6, 9.7
//! expect: reject
//! diagnostic: borrow-erasing
//! issue: 5659
module Conf;

public class Sink
  public static method K(x: Object)
  end K;
end Sink;

public class Client
  public static method Run()
    using var buf := new OwningBuffer<int>(4);
    var s := buf.AsSpan();
    Sink.K(s);
  end Run;
end Client;
