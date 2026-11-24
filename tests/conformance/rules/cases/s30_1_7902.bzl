//! case: s30_1_7902
//! doc: spec
//! section: 30.1
//! title: an async method with three string parameters and two nested generic awaits reads
//!   them all afterwards
//! rule: Calling an async method begins execution synchronously on the calling thread until
//!   the first `await` of an incomplete task, at which point the method suspends and returns
//!   its `Task` to the caller.
//! cites: 30.1
//! expect: run
//! stdout: 2170
module M;
class C
  public static async method Inner<T>(x: T, label: string): Task<int>
    await Task.Delay(1);
    return label.Length;
  end Inner;
  public static async method Outer(s: string, t: string?, u: string): Task<string>
    var a := await Inner<int>(3, "zz");
    var b := await Inner<string>("q" + s, u);
    await Task.Delay(1);
    var tl := 0;
    if t <> null then tl := t.Length; end;
    return s + u + (a + b + tl).ToString();
  end Outer;
  public static async method Main(): Task<int>
    var n := 0;
    for var k in 0..<200 do
      var r := await Outer("ab" + k.ToString(), "cd" + k.ToString(), "ef" + k.ToString());
      n := n + r.Length;
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;
