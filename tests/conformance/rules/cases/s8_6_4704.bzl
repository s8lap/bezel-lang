//! case: s8_6_4704
//! doc: spec
//! section: 8.6
//! title: a narrowed Guid? returned as Object and as IComparable<Guid> is a boxed Guid
//! rule: If the value is non-null, the result is a boxed `T` (the non-nullable form). The nullable
//!   wrapper is stripped during boxing.
//! cites: 5.1, 7.4, 2.6
//! expect: run
//! output: 00000000-0000-0000-0000-000000000000
//!   0
module Conf;

public class Source
  public method Mk(): Object
    var n: Guid? := Guid.Empty;
    return n;
  end Mk;

  public method MkI(): IComparable<Guid>
    var n: Guid? := Guid.Empty;
    return n;
  end MkI;
end Source;

public class Program
  public static method Main()
    var s := new Source();
    var o := s.Mk();
    Console.WriteLine(o.ToString());
    var i := s.MkI();
    Console.WriteLine(i.CompareTo(Guid.Empty));
  end Main;
end Program;
