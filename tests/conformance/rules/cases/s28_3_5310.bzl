//! case: s28_3_5310
//! doc: spec
//! section: 28.3
//! title: a closure over this reads an owned choice field as a borrow
//! rule: Field access `obj.f` where `f` is `owned` | Borrowed (source: `obj`)
//! cites: 28.3.4, 28.9, 35.5
//! expect: run
//! output: 200 200 1400
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

public class Holder
  public owned F: OC := OC.Text(new Res());

  public constructor() end constructor;

  public method ReadF(): int
    var f := lambda(): int => (case F of Text(r) => r.N, _ => 0);
    return f();
  end ReadF;
end Holder;

public class Program
  public static method T(i: int): int
    using var h := new Holder();
    return h.ReadF();
  end T;
  public static method Main()
    var s := 0;
    for var i in 0..<200 do
      s := s + T(i);
    end;
    Console.WriteLine($"{Res.Made} {Res.Count} {s}");
  end Main;
end Program;
