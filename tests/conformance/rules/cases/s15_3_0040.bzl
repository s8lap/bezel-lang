//! case: s15_3_0040
//! doc: spec
//! section: 15.3
//! title: Plain read of a field is accepted: a generic-class field passed from inside the generic body
//! rule: The field-take rejection applies to a field of move-only type only (spec 15.3 Parameter passing, K1.5); the form below reads, copies, matches or compares in place, consumes nothing, and the holder disposes each owned slot exactly once. The body type-checks once; substitutions int and a class are not move-only.
//! cites: 15.3, 25.2, 28.3
//! expect: run
//! output: 100
module M;
class Res
  public static _n: int := 0;
  public constructor() end constructor;
  destructor Res._n := Res._n + 1; end destructor;
end Res;
choice OChoice Text(owned r: Res), Empty;
  public method Describe(): int => 7;
end OChoice;
choice VC A(x: int), B; end VC;
choice BC P(r: Res), Q; end BC;
record Pt(x: int, y: int);
class Bx
  public owned V: OChoice := OChoice.Empty;
  public owned NV: OChoice? := null;
  public A: [2 owned]Res?;
  public C: VC := VC.B;
  public N: int := 5;
  public S: string := "hi";
  public P: Pt := new Pt(1, 2);
  public IA: [3]int;
  public Bc: BC := BC.Q;
  public owned R: Res := new Res();
  public Cls: Res? := null;
  public F: (int) -> int;
  public constructor()
    V := OChoice.Text(new Res()); NV := OChoice.Text(new Res());
    A := [new Res(), null];
    F := lambda(x: int) => x + 1;
  end constructor;
end Bx;
class Holder3<T> public owned F: T; public constructor(owned f: T) F := f; end constructor; public method Pass() Program.Id(F); Program.Id(this.F); end Pass; end Holder3; 
class Program
public static method Id<T>(x: T) end Id;
  public static method Body()
var h := new Holder3<int>(3); h.Pass(); var h2 := new Holder3<Res>(new Res()); h2.Pass();
  end Body;
  public static method Main()
    var i := 0;
    while i < 100 do Body(); i := i + 1; end while;
    Console.WriteLine(Res._n);
  end Main;
end Program;
