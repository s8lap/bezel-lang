//! case: s28_10_0037
//! doc: spec
//! section: 28.10
//! title: Unmarked using var over an adopted owned local with `move` pins and disposes nothing
//! rule: A `using var` initialized from a borrow does not assume a disposal obligation: at scope end no Dispose() is called, because the value is not owned by this scope; it only pins the source (spec 28.10, `using var` initialized from a borrow). The marked form `await using var y := move a;` adopts: y disposes the value once at its scope end.
//! cites: 28.10, 28.3
//! expect: run
//! output: END
//!   DTOR
module M; public class Res   public constructor() end constructor;   destructor Console.WriteLine("DTOR"); end destructor; end Res; public interface IThing end IThing; public class Thing: IThing   public constructor() end constructor;   destructor Console.WriteLine("TDTOR"); end destructor; end Thing; public record Rec(n: int); public class Holder   public owned R: Res := new Res();   public property P: Res => R;   public method Get(): Res => R;   public constructor() end constructor;   public method Self() using var x := R; using var y := this.R; Console.WriteLine("self"); end Self; end Holder;  public class Program public static async method Main(): Task var a := new Res(); await using var y := move a; Console.WriteLine("END"); end Main; end Program;
