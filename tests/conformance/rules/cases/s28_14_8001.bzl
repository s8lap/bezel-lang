//! case: s28_14_8001
//! doc: spec
//! section: 28.14
//! title: a capturing lambda passed to an async callee and awaited returns the captured text
//!   length plus five
//! rule: holds a plain function-typed parameter in that frame, and frame residence is not an
//!   escape sink
//! cites: 28.14, 30.1, 27.5
//! expect: run
//! stdout: 1690
//! notes: Pins #8950 (the counted cell of a lambda literal at an async callee's call-bounded
//!   formal); the corpus runner has no leak leg, so this asserts the program output.
module Conf;
public class C
  public static async method Later(h: (int) -> int): Task<int>
    await Task.Delay(1);
    return h(5);
  end Later;
  public static async method Main(): Task
    var a := 0;
    for var k in 0..<200 do
      var s := "x" + k.ToString();
      a := a + await Later(lambda(v: int): int => v + s.Length);
    end;
    Console.WriteLine(a);
  end Main;
end C;
