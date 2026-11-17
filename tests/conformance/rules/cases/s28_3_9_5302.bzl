//! case: s28_3_9_5302
//! doc: spec
//! section: 28.3.9
//! title: an outer move of an owned choice local after the capturing closure's last use is accepted
//! rule: Consuming an owned **binding** — a local or an `owned` parameter — while a live local borrow of
//!   its current value exists is a compile error
//! cites: 28.3.7, 28.3.9, 15.3
//! expect: run
//! output: 200 200 2800
module Conf;

public class Res
  public static Made: int := 0;
  public static Count: int := 0;
  public N: int := 7;

  public constructor()
    Res.Made := Res.Made + 1;
  end constructor;

  destructor
    Res.Count := Res.Count + 1;
  end destructor;
end Res;

public choice OC Text(owned r: Res), Empty; end OC;

public class Program
  public static method T(i: int): int
    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(r) => r.N, _ => 0);
    var k := f();
    var y := o;
    return k + (case y of Text(r) => r.N, _ => 0);
  end T;
  public static method Main()
    var s := 0;
    for var i in 0..<200 do
      s := s + T(i);
    end;
    Console.WriteLine($"{Res.Made} {Res.Count} {s}");
  end Main;
end Program;
