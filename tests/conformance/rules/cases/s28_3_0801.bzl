//! case: s28_3_0801
//! doc: spec
//! section: 28.3
//! title: owned temporaries in interpolation holes of return expressions are disposed before the return
//! rule: Such a temporary is disposed at the **end of the full expression** in which it is produced, after every use of it within that expression has completed
//! cites: 28.3.11, 18.5
//! expect: run
//! output: D1
//!   D5
//!   c1<c5>
//!   D1
//!   D5
//!   c1<c5>
//! issue: 8143
//! notes: a return operand is a full expression; the returned value is the string, so each hole's C is disposed (D1, D5) before the caller prints.
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
  public static method R1(): string => $"{C.Make()}";
  public static method R2(): string
    return $"<{new C(5)}>";
  end R2;
  public static method Main()
    var i := 0;
    while i < 2 do
      Console.WriteLine(R1() + R2());
      i := i + 1;
    end while;
  end Main;
end Program;
