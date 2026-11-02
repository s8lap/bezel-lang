//! case: s30_3_0018
//! doc: spec
//! section: 30.3
//! title: Wait called with an argument on a parenthesized task is rejected
//! rule: **Instance members win — by name.** If `x`'s declared type (or any
//!   base type) *declares* any instance member named `Foo` — of any arity,
//!   applicability, or accessibility — instance resolution is committed and
//!   extension blocks are **never consulted** for that name on that receiver.
//!   If the committed instance lookup then fails (no applicable overload,
//!   [§22.5](#sec-optional-parameters-and-named-arguments)/[§22.6](#sec-overloading),
//!   or the member is inaccessible, [§13.1](#sec-levels)), the call is a
//!   compile error citing the instance member(s); it does not fall back to an
//!   extension
//! cites: 30.3, 26.5, 22.6
//! expect: reject
//! diagnostic: Wait
module Conf;

public class Program
  public static method Main(): int
    using var t := Task.FromResult(2);
    (t).Wait(3);
    return 0;
  end Main;
end Program;
