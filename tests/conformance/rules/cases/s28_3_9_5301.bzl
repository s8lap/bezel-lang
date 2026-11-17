//! case: s28_3_9_5301
//! doc: spec
//! section: 28.3.9
//! title: an outer move of an owned choice local while a capturing closure is still live is rejected
//! rule: Consuming an owned **binding** — a local or an `owned` parameter — while a live local borrow of
//!   its current value exists is a compile error
//! cites: 28.3.7, 28.3.9, 15.3
//! expect: reject
//! diagnostic: borrow
module Conf;

public class Res
  public N: int := 7;

  public constructor() end constructor;

  destructor
    Console.WriteLine("released");
  end destructor;
end Res;

public choice OC Text(owned r: Res), Empty; end OC;

public class Program
  public static method T(i: int): int
    var o := OC.Text(new Res());
    var f := lambda(): int => (case o of Text(r) => r.N, _ => 0);
    var y := o;
    return f() + (case y of Text(r) => r.N, _ => 0);
  end T;
  public static method Main()
    var s := 0;
    for var i in 0..<200 do
      s := s + T(i);
    end;
    Console.WriteLine(s);
  end Main;
end Program;
