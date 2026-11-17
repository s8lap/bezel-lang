//! case: s22_6_2316
//! doc: spec
//! section: 22.6
//! title: a narrowed Guid? used as a Dictionary<Guid,int> key stores under the bare Guid
//! rule: a read of a binding [§7.4](#sec-member-access-on-nullable-values) has narrowed non-null
//!   is of its non-nullable type
//! cites: 22.6, 7.4, 8.6
//! expect: run
//! stdout: 5
//! issue: 8607
module Conf;

public class Program
  public static method Main()
    var n: Guid? := Guid.Empty;
    using var m := new Dictionary<Guid, int>();
    m[n] := 5;
    Console.WriteLine(m[Guid.Empty]);
  end Main;
end Program;
