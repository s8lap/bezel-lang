//! case: s17_8_0009
//! doc: spec
//! section: 17.8
//! title: for-in over a nested null-forgiving expression of a nullable Array local sums each element
//! rule: x! of a non-null operand is the assertion with no effect on the value and (x!)! likewise (spec 17.8); the local is borrowed by the loop, and the box field stays null: 50 sums of 6.
//! cites: 17.8
//! expect: run
//! output: sum=300 t=0
module Conf;
public class Box
  public Arr: Array<int>?;
  public constructor() end constructor;
end Box;
public class Program
  public static method Main(): int
    var sum := 0; var i := 0;
    using var box := new Box();
    while i < 50 do
      var a: Array<int>? := [1, 2, 3];
      for var x in a!! do sum := sum + x; end;
      i := i + 1;
    end;
    var t := 0;
    if box.Arr <> null then for var y in box.Arr! do t := t + y; end; end;
    Console.WriteLine("sum=" + sum.ToString() + " t=" + t.ToString());
    return 0;
  end Main;
end Program;
