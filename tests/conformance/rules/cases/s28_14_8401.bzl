//! case: s28_14_8401
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback captured by a non-escaping closure is admitted and invoked
//! rule: captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively)
//! cites: 28.14, 27.5
//! expect: run
//! stdout: 3270
//! notes: Pins #8953 (admitting twin of the escape rejects s28_14_8404/8405). `FwdL` captures
//!   its call-bounded `g` in the local closure `h`, which is only invoked and never leaves
//!   the callee, so the capture is not one that `itself escapes the callee`. Per i, L = 3
//!   + digits(i) is the length of `abc{i}`; `h()` runs `g()` twice (2L) and `Run` adds
//!   `k()` (L): 3L. Sum over i = 0..199 of 3L = 3 * 1090 = 3270.
module Conf;
import System;
public class C
  public static method Id(g: () -> int): int
    return g();
  end Id;
  public static method FwdL(g: () -> int): int
    var h := lambda() => C.Id(g);
    return h() + h();
  end FwdL;
  public static method Run(i: int): int
    var m := $"abc{i}";
    var k := lambda() => m.Length;
    var r := C.FwdL(k);
    return r + k();
  end Run;
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      s := s + Run(i);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end C;
