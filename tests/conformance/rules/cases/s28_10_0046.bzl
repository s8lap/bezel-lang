//! case: s28_10_0046
//! doc: spec
//! section: 28.10
//! title: Unmarked using var over a field pin read by a case expression is a borrow pin
//! rule: `using var x := h.F` over an owned field read is a borrow pin and disposes nothing at scope end (spec 28.10, `using var` initialized from a borrow); the holder's own disposal destroys each payload exactly once. A borrowing match over the pin consumes nothing; two Res per H2 are destroyed once: 200.
//! cites: 28.10, 28.3, 35.5
//! expect: run
//! output: 200
module M; public class Res public static Count: int := 0; public constructor() end constructor; destructor Res.Count := Res.Count + 1; end destructor; end Res; public choice OC Text(owned r: Res), Empty; end OC; public class H2 public owned F: OC := OC.Text(new Res()); public A: [2 owned]Res? := [new Res(), null]; public constructor() end constructor; end H2; public class Outer public owned B: H2 := new H2(); public constructor() end constructor; end Outer;  public class Program public static method T(h: H2): int using var x := h.F; return (case x of OC.Text(r) => 1, OC.Empty => 0); end T;  public static method Main(): int for var i in 0..<100 do do var h := new H2(); var k := T(h); end; end; Console.WriteLine($"{Res.Count}"); return 0; end Main; end Program;
