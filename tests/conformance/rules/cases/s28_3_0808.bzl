//! case: s28_3_0808
//! doc: spec
//! section: 28.3
//! title: holes that read a local and an owned field are borrows and are not disposed by the statement
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after every use of it within that expression has completed
//! cites: 28.3.11, 18.5
//! expect: run
//! output: c9c7
//!   c9c7
//!   END
//!   D7
//!   D9
//! issue: 8143
//! notes: control: reverse declaration order at scope end (c, then h whose Inner is disposed with it).
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

public class Holder
  public owned Inner: C;
  public constructor()
    Inner := new C(9);
  end constructor;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    using var c := new C(7);
    var i := 0;
    while i < 2 do
      var s := $"{h.Inner}{c}";
      Console.WriteLine(s);
      i := i + 1;
    end while;
    Console.WriteLine("END");
  end Main;
end Program;
