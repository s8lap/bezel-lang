//! case: s28_14_8403
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback forwarded through non-escaping capturing closures is invoked
//! rule: captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively)
//! cites: 28.14, 27.5
//! expect: run
//! stdout: 3670
//! notes: Pins #8953. `Fwd2` adds 1 to `g()`; `FwdL` captures `g` in `h` which calls `Fwd2(g)`,
//!   so `h()` = L + 1 and `FwdL` = 2(L + 1); `Run` adds `k()` = L. Per i: 3L + 2. Sum = 3
//!   * 1090 + 2 * 200 = 3670.
module Conf;
import System;
public class C
  public static method Fwd2(g: () -> int): int
    var h := lambda() => g() + 1;
    return h();
  end Fwd2;
  public static method FwdL(g: () -> int): int
    var h := lambda() => C.Fwd2(g);
    return h() + h();
  end FwdL;
  public static method Run(i: int): int
    var m := $"abc{i}";
    var k := lambda() => m.Length;
    return C.FwdL(k) + k();
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
