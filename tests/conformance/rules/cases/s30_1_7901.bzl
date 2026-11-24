//! case: s30_1_7901
//! doc: spec
//! section: 30.1
//! title: an async method keeps its string parameter after awaiting a nested generic async
//!   call
//! rule: Calling an async method begins execution synchronously on the calling thread until
//!   the first `await` of an incomplete task, at which point the method suspends and returns
//!   its `Task` to the caller.
//! cites: 30.1
//! expect: run
//! stdout: 1090
module M;
class C
  public static async method Inner<T>(x: T): Task<int>
    await Task.Delay(1);
    return 1;
  end Inner;
  public static async method Outer(s: string): Task<int>
    var y := await Inner<int>(3);
    await Task.Delay(1);
    return y + s.Length;
  end Outer;
  public static async method Main(): Task<int>
    var a := 0;
    for var k in 0..<200 do
      a := a + await Outer("ab" + k.ToString());
    end;
    Console.WriteLine(a);
    return 0;
  end Main;
end C;
