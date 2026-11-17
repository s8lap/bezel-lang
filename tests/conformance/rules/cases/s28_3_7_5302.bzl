//! case: s28_3_7_5302
//! doc: spec
//! section: 28.3.7
//! title: a closure that case-moves a captured owned choice local is rejected
//! rule: assignment to it, a marked transfer from it, an explicit `Dispose()` or discard of it, and every
//!   other owned-consuming use of it are compile errors
//! cites: 28.3.7, 27.5, 28.9
//! expect: reject
//! diagnostic: captured owned binding
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
    var f := lambda(): int => (case move o of Text(r) => 1, _ => 2);
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
