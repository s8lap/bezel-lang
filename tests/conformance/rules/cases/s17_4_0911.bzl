//! case: s17_4_0911
//! doc: spec
//! section: 17.4
//! title: as on a bare Guid target and a nullable int target unboxes and answers present
//! rule: the boxed `Object`-or-interface input is unboxed to the value type if compatible, and
//!   the result is `null` otherwise
//! cites: 17.4, 12.7
//! expect: run
//! output: True
//!   True
//! issue: 8593
module Conf;

public class Program
  public static method Main()
    var g: IComparable<Guid> := Guid.Empty;
    var x: Guid? := g as Guid;
    Console.WriteLine(x <> null);
    var i: int? := 5;
    var o: Object := i;
    var y: int? := o as int?;
    Console.WriteLine(y <> null);
  end Main;
end Program;
