//! case: s28_10_0047
//! doc: spec
//! section: 28.10
//! title: Using var over a nullable owned field pins and disposes nothing
//! rule: `using var x := h.G` over an owned field read, however spelled, is a borrow pin and disposes nothing at scope end (spec 28.10, `using var` initialized from a borrow); the holder's own disposal destroys each of the three Res (F's payload, G's payload, A[0]) exactly once, after END. 
//! cites: 28.10, 28.3
//! expect: run
//! output: in
//!   END
//!   DTOR
//!   DTOR
//!   DTOR
module M; public class Res public constructor() end constructor; destructor Console.WriteLine("DTOR"); end destructor; end Res; public choice OC Text(owned r: Res), Empty; end OC; public class H2 public owned F: OC := OC.Text(new Res()); public owned G: OC? := OC.Text(new Res()); public A: [2 owned]Res? := [new Res(), null]; public property P: OC => F; public constructor() end constructor; end H2;  public class Program public static method Main(): int var h := new H2(); do using var x := h.G; Console.WriteLine("in"); end; Console.WriteLine("END"); return 0; end Main; end Program;
