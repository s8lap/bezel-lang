//! case: s28_2_0024
//! doc: spec
//! section: 28.2
//! title: a class field named Dispose is rejected
//! rule: Declaring a member named `Dispose` on a class, a record, an enum or a
//!   choice is a compile error at that declaration, whatever its parameter list
//!   and whatever its kind — a method, instance or `static`, a field, a
//!   property, a `const`, an enum constant, a choice case or a nested type.
//! cites: 28.2, 9.2
//! expect: reject
//! diagnostic: dispose
//! issue: 7914
module Conf;

public class Gauge
  public Dispose: int := 0;

  public constructor()
  end constructor;
end Gauge;
