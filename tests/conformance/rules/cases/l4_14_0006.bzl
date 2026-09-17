//! case: l4_14_0006
//! doc: stdlib
//! section: 4.14
//! title: a Span<T> may not be stored in a static field unless it is the null-source form
//! rule: A borrow-bearing record may not be stored in a static field of any
//!   type, except when every reference-typed field of the value is null;
//!   Span<T>'s Buffer field is a non-nullable reference, so no Span<T>
//!   value ever satisfies the null-source form.
//! cites: 4.14, 9.7
//! expect: reject
//! diagnostic: static field
module Conf;

public class Cache
  private static owned readonly _buf: OwningBuffer<int> := new OwningBuffer<int>(1);
  private static readonly _view: Span<int> := _buf.AsSpan();
end Cache;
