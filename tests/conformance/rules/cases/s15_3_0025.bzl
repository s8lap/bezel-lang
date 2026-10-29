//! case: s15_3_0025
//! doc: spec
//! section: 15.3
//! title: Move-only field read at a plain call argument is rejected: an interpolation hole call
//! rule: A parameter declared `(x: V)` for a move-only V is by value-move, and a move-only field is never moved-from, so a plain read of it at such an argument is rejected; take it with the replacing `move <field> with <replacement>` (spec 15.3 Parameter passing, 28.8.1, Appendix C.3, K1.5). The interpolation-hole call position is the same position.
//! cites: 15.3, 28.8.1, 25.2
//! expect: reject
//! diagnostic: never moved-from
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
public static method Count(src: OChoice): int using var k := src; return 1; end Count; public static method Id2(n: int): int => n;
  public static method Body()
var bx := new Bx(); var b := $"{Count(bx.V)}";
  end Body;
  public static method Main()
    var i := 0;
    while i < 100 do Body(); i := i + 1; end while;
    Console.WriteLine(Res._n);
  end Main;
end Program;
