//! case: s15_3_0058
//! doc: spec
//! section: 15.3
//! title: An owned class field read at an `owned` argument in a condition is rejected as a borrow
//! rule: An owned class field read at an `owned` parameter is a borrow where an owned value is required: a borrow cannot be passed to an `owned` sink, and the owned field is taken with the replacing `move <field> with <replacement>` (spec 28.8.1 and 22.3 owned parameters, v0.350).
//! cites: 28.8.1, 22.3
//! expect: reject
//! diagnostic: owned
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
record Pt(x: int, y: int);
class Bx
  public owned V: OChoice := OChoice.Empty;
  public owned NV: OChoice? := null;
  public A: [2 owned]Res?;
  public C: VC := VC.B;
  public N: int := 5;
  public S: string := "hi";
  public P: Pt := new Pt(1, 2);
  public owned R: Res := new Res();
  public F: (int) -> int;
  public FC: (VC) -> int;
  public constructor()
    V := OChoice.Text(new Res()); NV := OChoice.Text(new Res());
    A := [new Res(), null];
    F := lambda(x: int) => x + 1;
    FC := lambda(c: VC) => 1;
  end constructor;

end Bx;

class Program
  public static readonly SC: VC := VC.B;
  public static method Count(src: OChoice): int using var k := src; return 1; end Count;
  public static method CountO(owned src: OChoice): int using var k := src; return 1; end CountO;
  public static method CountN(src: OChoice?): int using var k := src; return 1; end CountN;
  public static method Keep(src: OChoice) using var k := src; end Keep;
  public static method KeepN(src: OChoice?) using var k := src; end KeepN;
  public static method Pk(c: VC): int => 1;
  public static method PeekR(r: Res): int => 1;
  public static method TakeR(owned r: Res): int using var k := move r; return 1; end TakeR;
  public static method Gi<T>(x: T): int => 1;
  public static method Two(n: int, src: OChoice): int using var k := src; return n; end Two;
  public static method Two2(c: VC, o: OChoice): int using var k := o; return 1; end Two2;
  public static method Make(): OChoice return OChoice.Text(new Res()); end Make;

  public static method Body()
var bx := new Bx(); if TakeR(bx.R) > 0 then Console.WriteLine(1); end if;
  end Body;
  public static method Main()
    var i := 0;
    while i < 100 do Body(); i := i + 1; end while;
    Console.WriteLine(Res._n);
  end Main;
end Program;
