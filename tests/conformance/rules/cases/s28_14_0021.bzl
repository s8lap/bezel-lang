//! case: s28_14_0021
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback stored to a bare auto-property identifier is rejected
//! rule: A function-typed parameter declared without scoped is call-bounded;
//!   the callback may not be stored in a field of any object (spec §28.14).
//!   A bare auto-property identifier resolves to PropertySymbol, not
//!   FieldSymbol, and was previously missed by the sink check.
//! cites: 28.14
//! expect: reject
//! diagnostic: call-bounded
module Conf;

public class C
  public property Prop: () -> ();

  public method Store(cb: () -> ())
    Prop := cb;
  end Store;
end C;
