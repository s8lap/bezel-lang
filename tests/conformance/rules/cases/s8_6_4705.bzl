//! case: s8_6_4705
//! doc: spec
//! section: 8.6
//! title: a narrowed Guid? stored through Object and interface-typed properties is a boxed Guid
//! rule: If the value is non-null, the result is a boxed `T` (the non-nullable form). The nullable
//!   wrapper is stripped during boxing.
//! cites: 5.1, 7.4, 2.6
//! expect: run
//! output: 00000000-0000-0000-0000-000000000000
//!   0
module Conf;

public class Holder
  public property P: Object;
  public property Q: IComparable<Guid>;

  public constructor()
    this.P := Guid.Empty;
    this.Q := Guid.Empty;
  end constructor;
end Holder;

public class Program
  public static method Main()
    var n: Guid? := Guid.Empty;
    var h := new Holder();
    h.P := n;
    h.Q := n;
    Console.WriteLine(h.P.ToString());
    Console.WriteLine(h.Q.CompareTo(Guid.Empty));
  end Main;
end Program;
