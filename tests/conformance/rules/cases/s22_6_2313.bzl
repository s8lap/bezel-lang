//! case: s22_6_2313
//! doc: spec
//! section: 22.6
//! title: a narrowed Guid? passed to a generic constructor that stores it is stored as the bare Guid
//! rule: a read of a binding [§7.4](#sec-member-access-on-nullable-values) has narrowed non-null
//!   is of its non-nullable type
//! cites: 22.6, 7.4
//! expect: run
//! output: True
//!   True
//! issue: 8607
module Conf;

public class Box<T>
  public v: T;

  public constructor(x: T)
    v := x;
  end constructor;
end Box;

public class Program
  public static method Main()
    var n: Guid? := Guid.Empty;
    var b := new Box<Guid>(n);
    Console.WriteLine(b.v = Guid.Empty);
    var d: DateTime? := new DateTime(2020, 1, 1);
    var b2 := new Box<DateTime>(d);
    Console.WriteLine(b2.v = new DateTime(2020, 1, 1));
  end Main;
end Program;
