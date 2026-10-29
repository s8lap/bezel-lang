//! case: s15_3_0042
//! doc: spec
//! section: 15.3
//! title: Plain read of a field is accepted: a borrowing case over an owning choice field
//! rule: The field-take rejection applies to a field of move-only type only (spec 15.3 Parameter passing, K1.5); the form below reads, copies, matches or compares in place, consumes nothing, and the holder disposes each owned slot exactly once. A borrowing match over a field consumes nothing (spec 19.3).
//! cites: 15.3, 25.2, 28.3
//! expect: run
//! output: 400
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

class Program

  public static method Body()
var bx := new Bx(); var n := (case bx.V of Text(r) => 1, Empty => 0); var m := (case bx.NV of Text(r) => 1, Empty => 2, null => 3);
  end Body;
  public static method Main()
    var i := 0;
    while i < 100 do Body(); i := i + 1; end while;
    Console.WriteLine(Res._n);
  end Main;
end Program;
