//! case: s9_7_0027
//! doc: spec
//! section: 9.7
//! title: a span passed at a bare T the call substitutes with Object is rejected
//! rule: A value that holds borrows may not reach a borrow-erasing position: a
//!   position declared `Object` or an interface type — a parameter, plain or
//!   `scoped`, a local, a field, a record component, a choice payload, an
//!   inline-array slot, a collection slot or collection-literal element, a
//!   `ref` or `out` location, a return slot — or a position declared at a
//!   bare type parameter that the instantiation substitutes with such a type
//!   (`b.Keep<Object>(span)` over `Keep<T>(x: T)`; `xs.Add(span)` at `xs:
//!   Array<Object>`).
//! cites: 9.7, 8.6
//! expect: reject
//! diagnostic: borrow-erasing
//! issue: 5659
module Conf;

public class Box
  private _f: Object?;

  public constructor()
  end constructor;

  public method Keep<T>(x: T)
    _f := x;
  end Keep;
end Box;

public class Client
  public static method Run(b: Box)
    using var buf := new OwningBuffer<int>(4);
    var s := buf.AsSpan();
    b.Keep<Object>(s);
  end Run;
end Client;
