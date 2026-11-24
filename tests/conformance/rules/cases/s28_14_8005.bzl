//! case: s28_14_8005
//! doc: spec
//! section: 28.14
//! title: an instance async callee given a lambda capturing a local and this yields the
//!   captured length
//! rule: holds a plain function-typed parameter in that frame, and frame residence is not an
//!   escape sink
//! cites: 28.14, 30.1, 27.5
//! expect: run
//! stdout: 690
//! notes: Pins #8950 for an instance callee and a lambda that also captures `this`.
module Conf;
public class C
  private _n: int := 0;
  public async method Later(h: (int) -> int): Task<int>
    await Task.Delay(1);
    return h(this._n);
  end Later;
  public async method Run(k: int): Task<int>
    var s := "x" + k.ToString();
    return await this.Later(lambda(v: int): int => v + s.Length + this._n);
  end Run;
  public static async method Main(): Task
    var a := 0;
    var c := new C();
    for var k in 0..<200 do
      a := a + await c.Run(k);
    end;
    Console.WriteLine(a);
  end Main;
end C;
