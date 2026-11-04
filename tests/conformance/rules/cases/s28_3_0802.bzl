//! case: s28_3_0802
//! doc: spec
//! section: 28.3
//! title: owned temporaries in holes of var and typed var initializers are disposed at the declaration's end
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after every use of it within that expression has completed
//! cites: 28.3.11, 18.5
//! expect: run
//! output: D1
//!   D5
//!   c5|<c1>
//!   D1
//!   D5
//!   c5|<c1>
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
    var i := 0;
    while i < 2 do
      var s := $"<{C.Make()}>";
      var u: string := $"{new C(5)}|{s}";
      Console.WriteLine(u);
      i := i + 1;
    end while;
  end Main;
end Program;
