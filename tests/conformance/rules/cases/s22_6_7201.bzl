//! case: s22_6_7201
//! doc: spec
//! section: 22.6
//! title: a constructor overload set resolves int and long second
//!   parameters beside a lambda argument
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot
//! cites: 22.6, 28.14
//! expect: run
//! stdout: 203270
module Conf;

public class K
  public v: int;

  public constructor(f: () -> int)
    v := f();
  end constructor;

  public constructor(f: () -> int, n: int)
    v := f() + n;
  end constructor;

  public constructor(f: () -> int, n: long)
    v := f() + 1000;
  end constructor;

  public constructor(o: Object, n: int, m: int)
    v := 7;
  end constructor;
end K;

public class Program
  public static method Main()
    var i := 0;
    var tot := 0;
    while i < 200 do
      var s := "hi" + i.ToString();
      using var a := new K(lambda(): int => s.Length);
      using var b := new K(lambda(): int => s.Length, 3);
      var l: long := 5;
      using var c := new K(lambda(): int => s.Length, l);
      tot := tot + a.v + b.v + c.v;
      i := i + 1;
    end while;
    Console.WriteLine(tot);
  end Main;
end Program;
