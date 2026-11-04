//! case: s28_3_0803
//! doc: spec
//! section: 28.3
//! title: owned temporaries in interpolation holes inside lambda bodies are disposed at each evaluation's end
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after every use of it within that expression has completed
//! cites: 28.3.11, 18.5
//! expect: run
//! output: D1
//!   D2
//!   c1c2:0
//!   D1
//!   D2
//!   c1c2:1
//!   D7
//! issue: 8143
//! notes: f() is evaluated before g(i); both lambdas dispose their own hole temporary before returning; the captured using var c is disposed at the end of Main.
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
  public static method Main()
    using var c := new C(7);
    var f := lambda() => $"{C.Make()}";
    var g := lambda(k: int) => do
      var s := $"{c.Fresh()}:{k}";
      return s;
    end;
    var i := 0;
    while i < 2 do
      Console.WriteLine(f() + g(i));
      i := i + 1;
    end while;
  end Main;
end Program;
