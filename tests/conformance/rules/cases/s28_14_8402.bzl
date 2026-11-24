//! case: s28_14_8402
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback invoked by a non-escaping closure that throws is caught
//! rule: captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively)
//! cites: 28.14, 27.5, 31.2
//! expect: run
//! stdout: 1090
//! notes: Pins #8953. The caller's closure `k` throws on its first call; `FwdL`'s non-escaping
//!   closure `h` forwards `g()`, so the exception unwinds through `h` and `FwdL` to the
//!   caller's `try`. The arm runs, `r` stays 0, and `Run` returns `r + m.Length` = L (the
//!   length of `abc{i}`). Sum over i = 0..199 of (3 + digits(i)) = 1090.
module Conf;
import System;
public class C
  public static method Nop(): int return 0; end Nop;
  public static method FwdL(g: () -> int): int
    var h := lambda() => g();
    return h();
  end FwdL;
  public static method Run(i: int): int
    var m := $"abc{i}";
    var k := lambda() => do
      if m.Length > 0 then throw new InvalidOperationException("boom"); end if;
      return 1;
    end;
    var r := 0;
    try
      r := C.FwdL(k);
    catch
      InvalidOperationException => C.Nop(),
    end try;
    return r + m.Length;
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
