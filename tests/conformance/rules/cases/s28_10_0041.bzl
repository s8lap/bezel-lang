//! case: s28_10_0041
//! doc: spec
//! section: 28.10
//! title: Unmarked using var over a static owning-array field is a borrow pin
//! rule: `using var x := h.F` over an owned field read is a borrow pin and disposes nothing at scope end (spec 28.10, `using var` initialized from a borrow); the holder's own disposal destroys each payload exactly once. The static field is never destroyed in the run, so the pin and the program disposes no Res: the count printed is 0.
//! cites: 28.10, 28.3, 35.5
//! expect: run
//! output: 0
module M; public class Res public static Count: int := 0; public constructor() end constructor; destructor Res.Count := Res.Count + 1; end destructor; end Res; public choice OC Text(owned r: Res), Empty; end OC; public class H2 public owned F: OC := OC.Text(new Res()); public A: [2 owned]Res? := [new Res(), null]; public constructor() end constructor; end H2; public class Outer public owned B: H2 := new H2(); public constructor() end constructor; end Outer; public class SH public static A: [2 owned]Res? := [new Res(), null]; end SH;  public class Program  public static method Main(): int for var i in 0..<100 do do using var y := SH.A; end; end; Console.WriteLine($"{Res.Count}"); return 0; end Main; end Program;
