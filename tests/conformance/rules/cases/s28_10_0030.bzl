//! case: s28_10_0030
//! doc: spec
//! section: 28.10
//! title: Unmarked using var over a `??` of two parenthesized null-forgiving owned reads is rejected
//! rule: An unmarked `using var y := x;` is a compile error when its right-hand side is a named owned binding whose declared type, nullability stripped, is a class or interface type: it says neither that y adopts x nor that it views it (spec 28.10, Adopting a named owned binding); `move x` adopts, a `var` or `const` binding views. `(x)` and `x!` are classified as the bare `x`, in a `??` meet whose every arm is one.
//! cites: 28.10, 28.12, 21.1
//! expect: reject
//! diagnostic: adopts
module M; public class Res   public constructor() end constructor;   destructor Console.WriteLine("DTOR"); end destructor; end Res; public interface IThing end IThing; public class Thing: IThing   public constructor() end constructor;   destructor Console.WriteLine("TDTOR"); end destructor; end Thing; public record Rec(n: int); public class Holder   public owned R: Res := new Res();   public property P: Res => R;   public method Get(): Res => R;   public constructor() end constructor;   public method Self() using var x := R; using var y := this.R; Console.WriteLine("self"); end Self; end Holder;  public class Program public static method Main(): int var a: Res? := new Res(); var b: Res? := new Res(); using var x := ((a!) ?? (b!)); Console.WriteLine("END"); return 0;  end Main; end Program;
