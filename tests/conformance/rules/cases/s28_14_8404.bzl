//! case: s28_14_8404
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback captured by a closure that is itself returned is rejected
//! rule: captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively)
//! cites: 28.14, 28.3
//! expect: reject
//! diagnostic: call-bounded
//! notes: Pins #8953's boundary: the admission is for a closure that does not leave the callee.
//!   Here `h` captures `g` and the returned closure captures `h`, so a capture of `g`
//!   escapes the callee transitively. Twin: s28_14_8401 (the closure `h` is only invoked
//!   inside the callee).
module Conf;
import System;
public class C
  public static method FwdL(g: () -> int): () -> int
    var h := lambda() => g();
    return lambda() => h();
  end FwdL;
  public static method Main(): int
    var m := "abc";
    var k := lambda() => m.Length;
    var f := C.FwdL(k);
    Console.WriteLine(f());
    return 0;
  end Main;
end C;
