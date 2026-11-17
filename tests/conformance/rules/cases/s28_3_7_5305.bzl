//! case: s28_3_7_5305
//! doc: spec
//! section: 28.3.7
//! title: a closure capturing an owned choice local may not be returned from the local's scope
//! rule: the closure captures a borrow whose source is the local's enclosing scope, and may not outlive
//!   it.
//! cites: 28.3.7, 28.9, 27.5
//! expect: reject
//! diagnostic: captures
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
  public static method Mk(): () -> int
    var o := OC.Text(new Res());
    return lambda(): int => (case o of Text(r) => r.N, _ => 0);
  end Mk;

  public static method T(i: int): int
    var f := Program.Mk();
    return f();
  end T;
  public static method Main()
    var s := 0;
    for var i in 0..<200 do
      s := s + T(i);
    end;
    Console.WriteLine(s);
  end Main;
end Program;
