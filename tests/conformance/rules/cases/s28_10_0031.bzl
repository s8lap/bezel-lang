//! case: s28_10_0031
//! doc: spec
//! section: 28.10
//! title: Unmarked using var over a plain borrowed parameter pins and disposes nothing
//! rule: A `using var` initialized from a borrow does not assume a disposal obligation: at scope end no Dispose() is called, because the value is not owned by this scope; it only pins the source (spec 28.10, `using var` initialized from a borrow). A plain parameter is borrow state, so the pin disposes nothing and the caller's local is disposed once at its scope end, after END.
//! cites: 28.10, 28.3
//! expect: run
//! output: END
//!   DTOR
module M; public class Res   public constructor() end constructor;   destructor Console.WriteLine("DTOR"); end destructor; end Res; public interface IThing end IThing; public class Thing: IThing   public constructor() end constructor;   destructor Console.WriteLine("TDTOR"); end destructor; end Thing; public record Rec(n: int); public class Holder   public owned R: Res := new Res();   public property P: Res => R;   public method Get(): Res => R;   public constructor() end constructor;   public method Self() using var x := R; using var y := this.R; Console.WriteLine("self"); end Self; end Holder;  public class Program public static method Look(src: Res) using var x := src; end Look; public static method Main(): int var r := new Res(); Program.Look(r); Console.WriteLine("END"); return 0;  end Main; end Program;
