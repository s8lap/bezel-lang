//! case: s22_6_2315
//! doc: spec
//! section: 22.6
//! title: a narrowed Guid? passed to generic Keep<Guid> and Id<DateTime> keeps the bare value in the result
//! rule: a read of a binding [§7.4](#sec-member-access-on-nullable-values) has narrowed non-null
//!   is of its non-nullable type
//! cites: 22.6, 7.4, 14.5
//! expect: run
//! output: True
//!   True
//!   True
//! issue: 8607
module Conf;

public static class Gen
  public static method Id<T>(x: T): T
    return x;
  end Id;

  public static method Keep<T>(x: T): owned Array<T>
    var a := new Array<T>();
    a.Add(x);
    return a;
  end Keep;
end Gen;

public class Program
  public static method Main()
    var n: Guid? := Guid.Empty;
    using var a := Gen.Keep<Guid>(n);
    Console.WriteLine(a[0] = Guid.Empty);
    var d: DateTime? := new DateTime(2020, 1, 1);
    Console.WriteLine(Gen.Id<DateTime>(d) = new DateTime(2020, 1, 1));
    var o: Object := Gen.Id<DateTime>(d);
    Console.WriteLine(o.Equals(new DateTime(2020, 1, 1)));
  end Main;
end Program;
