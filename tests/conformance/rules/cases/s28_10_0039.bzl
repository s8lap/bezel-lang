//! case: s28_10_0039
//! doc: spec
//! section: 28.10
//! title: Unmarked using var over a mixed `if` meet of an owned local and a borrowed parameter pins and dispos
//! rule: A `using var` initialized from a borrow does not assume a disposal obligation: at scope end no Dispose() is called, because the value is not owned by this scope; it only pins the source (spec 28.10, `using var` initialized from a borrow). A meet with a borrow arm is not covered; the pin disposes nothing, so each owner disposes its own value once.
//! cites: 28.10, 28.3
//! expect: run
//! output: DTOR
//!   END
//!   DTOR
module M; public class Res   public constructor() end constructor;   destructor Console.WriteLine("DTOR"); end destructor; end Res; public interface IThing end IThing; public class Thing: IThing   public constructor() end constructor;   destructor Console.WriteLine("TDTOR"); end destructor; end Thing; public record Rec(n: int); public class Holder   public owned R: Res := new Res();   public property P: Res => R;   public method Get(): Res => R;   public constructor() end constructor;   public method Self() using var x := R; using var y := this.R; Console.WriteLine("self"); end Self; end Holder;  public class Program public static method M2(p: Res, c: bool) var a := new Res(); using var x := (if c then a else p); end M2; public static method Main(): int var r := new Res(); Program.M2(r, true); Console.WriteLine("END"); return 0;  end Main; end Program;
