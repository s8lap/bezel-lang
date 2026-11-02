//! case: s30_3_0020
//! doc: spec
//! section: 30.3
//! title: Wait called with an argument through a null-conditional chain is rejected
//! rule: **Instance members win — by name.** If `x`'s declared type (or any
//!   base type) *declares* any instance member named `Foo` — of any arity,
//!   applicability, or accessibility — instance resolution is committed and
//!   extension blocks are **never consulted** for that name on that receiver.
//!   If the committed instance lookup then fails (no applicable overload,
//!   [§22.5](#sec-optional-parameters-and-named-arguments)/[§22.6](#sec-overloading),
//!   or the member is inaccessible, [§13.1](#sec-levels)), the call is a
//!   compile error citing the instance member(s); it does not fall back to an
//!   extension
//! cites: 30.3, 26.5, 22.6, 7.5
//! expect: reject
//! diagnostic: Wait
module Conf;

public class Holder
  public Cell: Task<int>? := null;
end Holder;

public class Program
  public static method Main(): int
    var h: Holder? := new Holder();
    h?.Cell?.Wait(3);
    return 0;
  end Main;
end Program;
