//! case: s22_6_2314
//! doc: spec
//! section: 22.6
//! title: a narrowed Guid? and DateTime? added to Array<Guid> and Array<DateTime> are stored as bare values
//! rule: a read of a binding [§7.4](#sec-member-access-on-nullable-values) has narrowed non-null
//!   is of its non-nullable type
//! cites: 22.6, 7.4
//! expect: run
//! output: True
//!   True
//! issue: 8607
module Conf;

public class Program
  public static method Main()
    var n: Guid? := Guid.Empty;
    using var l := new Array<Guid>();
    l.Add(n);
    Console.WriteLine(l[0] = Guid.Empty);
    var d: DateTime? := new DateTime(2020, 1, 1);
    using var l2 := new Array<DateTime>();
    l2.Add(d);
    Console.WriteLine(l2[0] = new DateTime(2020, 1, 1));
  end Main;
end Program;
