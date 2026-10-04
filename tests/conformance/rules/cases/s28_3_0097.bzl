//! case: s28_3_0097
//! doc: spec
//! section: 28.3
//! title: a call-bounded callback stored to a bare auto-property identifier is rejected
//! rule: A property or indexer assignment of a function-typed value with a
//!   non-empty source set is rejected at the site (spec §28.3, v0.314).
//!   A bare auto-property identifier resolves to PropertySymbol, not
//!   FieldSymbol, and was previously missed by the sink check.
//! cites: 28.3, 28.14
//! expect: reject
//! diagnostic: call-bounded
module Conf;

public class C
  public property Prop: () -> ();

  public method Store(cb: () -> ())
    Prop := cb;
  end Store;
end C;
