//! case: s28_14_8003
//! doc: spec
//! section: 28.14
//! title: a callee that throws after invoking the captured lambda is caught and counted 200
//!   times
//! rule: holds a plain function-typed parameter in that frame, and frame residence is not an
//!   escape sink
//! cites: 28.14, 30.1
//! expect: run
//! stdout: 200
//! notes: Pins #8950's throw path: the cell's release must also happen when the awaited
//!   callee faults.
module Conf;
public class C
  public static Faults: int := 0;
  public static method Note(): int
    C.Faults := C.Faults + 1;
    return 0;
  end Note;
  public static async method Later(h: (int) -> int): Task<int>
    await Task.Delay(1);
    if h(5) > 0 then throw new Exception("boom"); end;
    return 1;
  end Later;
  public static async method Main(): Task
    var a := 0;
    for var k in 0..<200 do
      var s := "x" + k.ToString();
      try
        a := a + await Later(lambda(v: int): int => v + s.Length);
      catch
        var e: Exception => C.Note(),
      end try;
    end;
    Console.WriteLine(C.Faults + a);
  end Main;
end C;
