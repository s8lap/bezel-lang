//! case: s28_3_0807
//! doc: spec
//! section: 28.3
//! title: a hole whose value is a borrowed result (c.Self()) is not an owned temporary and is not disposed
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after every use of it within that expression has completed
//! cites: 28.3.11, 18.5
//! expect: run
//! output: c7
//!   c7
//!   END 7
//!   D7
//! issue: 8143
//! notes: control: only owned unbound temporaries are disposed at the boundary; c is disposed once at the end of Main.
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
    var i := 0;
    while i < 2 do
      var s := $"{c.Self()}";
      Console.WriteLine(s);
      i := i + 1;
    end while;
    Console.WriteLine("END " + c.Id.ToString());
  end Main;
end Program;
