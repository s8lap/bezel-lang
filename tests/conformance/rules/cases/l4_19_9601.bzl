//! case: l4_19_9601
//! doc: stdlib
//! section: 4.19
//! title: reading FrozenArray's Length is admitted and reports the element count
//! rule: `Length` measures a fixed or contiguous extent
//! cites: 1
//! expect: run
//! stdout: 2
//! issue: 9214
module Conf;

public class Program
  public static method Main()
    var s := new Array<int>();
    s.Add(7);
    s.Add(2);
    using var f := FrozenArray<int>.From(s);
    var x := f.Length;
    Console.WriteLine(x);
  end Main;
end Program;
