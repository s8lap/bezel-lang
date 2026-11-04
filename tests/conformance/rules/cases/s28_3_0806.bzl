//! case: s28_3_0806
//! doc: spec
//! section: 28.3
//! title: a null-conditional call in a hole yields a disposed temporary, or the text null with nothing to free
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after every use of it within that expression has completed
//! cites: 28.3.11, 18.5
//! expect: run
//! output: D2
//!   c2|null
//!   D2
//!   c2|null
//!   D7
//! issue: 8143
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
    using var a: C? := new C(7);
    var b: C? := null;
    var i := 0;
    while i < 2 do
      var s := $"{a?.Fresh()}|{b?.Fresh()}";
      Console.WriteLine(s);
      i := i + 1;
    end while;
  end Main;
end Program;
