//! case: s15_3_7653
//! doc: spec
//! section: 15.3
//! title: a move-only parameter forwarded to one of two callees is disposed once
//! rule: for a move-only V, it is by value-move (transfer ownership)
//! cites: 15.3, 28.3.10
//! expect: run
//! stdout: 200
module Conf;
import System;
class Res
  public static N: int := 0;
  public constructor() end constructor;
  destructor Res.N := Res.N + 1; end destructor;
end Res;
choice OChoice Text(owned r: Res), Empty;
  public method Describe(): int => 7;
end OChoice;
class Program
  public static method A(o: OChoice): int return 1; end A;
  public static method B(o: OChoice): int return 2; end B;
  public static method T(o: OChoice, c: bool): int
    if c then return A(o); end;
    return B(o);
  end T;
  public static method Main()
    var i := 0;
    while i < 200 do var k := T(OChoice.Text(new Res()), i mod 2 = 0); i := i + 1; end while;
    Console.WriteLine(Res.N);
  end Main;
end Program;
