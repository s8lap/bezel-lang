//! case: s28_3_0805
//! doc: spec
//! section: 28.3
//! title: owned hole temporaries with a format specifier or alignment are disposed at the declaration's end
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after every use of it within that expression has completed
//! cites: 28.3.11, 18.5
//! expect: run
//! output: DF
//!   D1
//!   D5
//!   [3.14]
//!   [      c1]
//!   [c5    ]
//! issue: 8143
//! notes: one temporary per declaration, so the disposal order is deterministic; alignment right-aligns for a positive width and left-aligns for a negative one (18.5).
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

public class F: IFormattable
  public V: double;
  public constructor(v: double)
    V := v;
  end constructor;
  destructor
    Console.WriteLine("DF");
  end destructor;
  public override method ToString(): string => "F";
  public method ToString(format: string): string => V.ToString(format);
  public static method Make(): owned F
    return new F(3.14159);
  end Make;
end F;

public class Program
  public static method Main()
    var a := $"[{F.Make():F2}]";
    var b := $"[{C.Make(),8}]";
    var d := $"[{new C(5),-6}]";
    Console.WriteLine(a);
    Console.WriteLine(b);
    Console.WriteLine(d);
  end Main;
end Program;
