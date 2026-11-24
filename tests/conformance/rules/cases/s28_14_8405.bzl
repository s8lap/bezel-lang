//! case: s28_14_8405
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback captured by a closure stored in a field of an object is
//!   rejected
//! rule: captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively)
//! cites: 28.14, 23.1
//! expect: reject
//! diagnostic: call-bounded
//! notes: Pins #8953's boundary with the store spelling: `h` captures `g` and `h` is assigned
//!   to the holder's property, so the closure escapes the callee through the holder. Twin:
//!   s28_14_8401.
module Conf;
import System;
public class Holder
  public property F: (() -> int)?;
  public constructor() end constructor;
end Holder;
public class C
  public static method FwdL(g: () -> int, hd: Holder): int
    var h := lambda() => g();
    hd.F := h;
    return h();
  end FwdL;
  public static method Main(): int
    using var hd := new Holder();
    var m := "abc";
    var k := lambda() => m.Length;
    Console.WriteLine(C.FwdL(k, hd));
    return 0;
  end Main;
end C;
