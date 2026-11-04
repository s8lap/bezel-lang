//! case: s28_3_0804
//! doc: spec
//! section: 28.3
//! title: an owned hole temporary is disposed once during unwinding when a later hole of the expression throws
//! rule: unbound owned temporary | end of the full expression; during unwinding if the expression is exited by an exception
//! cites: 28.3.13, 28.3.11, 18.5
//! expect: run
//! output: D1
//!   D1
//!   c1ok
//!   D1
//!   caught=2
//! issue: 8143
//! notes: i=0 and i=2 throw in the second hole: the first hole's C is disposed by the unwind (one D1 each); i=1 completes and disposes at the end of the declaration.
module Conf;

public class C
  public Id: int;
  public constructor(id: int)
    Id := id;
  end constructor;
  destructor
    Console.WriteLine("D" + Id.ToString());
  end destructor;
  public override method ToString(): string => "c" + Id.ToString();
  public static method Make(): owned C
    return new C(1);
  end Make;
  public method Fresh(): owned C
    return new C(2);
  end Fresh;
  public method Self(): C => this;
end C;

public class Program
  public static method Boom(k: int): string
    if k mod 2 = 0 then
      throw new InvalidOperationException("boom");
    end;
    return "ok";
  end Boom;
  public static method Main()
    var i := 0;
    var caught := 0;
    while i < 3 do
      try
        var s := $"{C.Make()}{Boom(i)}";
        Console.WriteLine(s);
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("caught=" + caught.ToString());
  end Main;
end Program;
