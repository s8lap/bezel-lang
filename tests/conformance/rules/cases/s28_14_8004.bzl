//! case: s28_14_8004
//! doc: spec
//! section: 28.14
//! title: an early return and a break around the awaited call leave the lambda results exact
//! rule: holds a plain function-typed parameter in that frame, and frame residence is not an
//!   escape sink
//! cites: 28.14, 30.1
//! expect: run
//! stdout: 3481
//! notes: Pins #8950 on the break and early-return exits of the frame holding the counted
//!   cell.
module Conf;
public class C
  public static async method Later(h: (int) -> int): Task<int>
    await Task.Delay(1);
    return h(5);
  end Later;
  public static async method Inner(k: int): Task<int>
    var s := "x" + k.ToString();
    var r := await Later(lambda(v: int): int => v + s.Length);
    if k > 100 then return r; end;
    return r + 1;
  end Inner;
  public static async method Main(): Task
    var a := 0;
    for var k in 0..<400 do
      var s := "x" + k.ToString();
      var r := await Later(lambda(v: int): int => v + s.Length);
      if k >= 200 then break; end;
      a := a + r;
    end;
    for var k in 0..<200 do
      a := a + await Inner(k);
    end;
    Console.WriteLine(a);
  end Main;
end C;
