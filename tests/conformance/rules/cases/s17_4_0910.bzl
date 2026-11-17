//! case: s17_4_0910
//! doc: spec
//! section: 17.4
//! title: a 300-round loop of as Guid? and as DateTime? over boxed values counts every present result
//! rule: the boxed `Object`-or-interface input is unboxed to the value type if compatible, and
//!   the result is `null` otherwise
//! cites: 17.4, 12.7
//! expect: run
//! stdout: 600
//! issue: 8593
module Conf;

public class Program
  public static method Main()
    var sum := 0;
    for var i in 0..<300 do
      var o: Object := Guid.Empty;
      var x: Guid? := o as Guid?;
      if x <> null then
        sum := sum + 1;
      end;
      var d: IComparable<DateTime> := new DateTime(2020, 1, 1);
      var y: DateTime? := d as DateTime?;
      if y <> null then
        sum := sum + 1;
      end;
    end;
    Console.WriteLine(sum);
  end Main;
end Program;
