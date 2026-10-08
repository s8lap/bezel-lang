//! case: s9_7_0028
//! doc: spec
//! section: 9.7
//! title: a span added to an Array<Object> is rejected at the conversion
//! rule: A value that holds borrows may not reach a borrow-erasing position: a
//!   position declared `Object`, or an interface type other than
//!   `IReadOnlySpan<TElement>` — a parameter, plain or `scoped`, a local, a
//!   field, a record component, a choice payload, an inline-array slot, a
//!   collection slot or collection-literal element, a `ref` or `out`
//!   location, a return slot — or a position declared at a bare type
//!   parameter that the instantiation substitutes with such a type
//!   (`b.Keep<Object>(span)` over `Keep<T>(x: T)`; `xs.Add(span)` at `xs:
//!   Array<Object>`).
//! cites: 9.7, 8.6, 28.11
//! expect: reject
//! diagnostic: borrow-erasing
//! issue: 5659
module Conf;

public class Client
  public static method Run(): int
    using var buf := new OwningBuffer<int>(4);
    var s := buf.AsSpan();
    using var xs := new Array<Object>();
    xs.Add(s);
    return xs.Count;
  end Run;
end Client;
