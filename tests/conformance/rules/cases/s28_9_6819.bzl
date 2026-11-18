//! case: s28_9_6819
//! doc: spec
//! section: 28.9
//! title: a function assigned from `n ?? g` before an await and stored after it stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 12.7, 7.7
//! expect: run
//! stdout: 600
//! notes: A suspension between the bind and the store changes nothing: 600 = 200 iterations of g(5, 2) = 3.
module Conf;

type Fn := (int, int) -> int;

public class U
  public static method T(o: Object?): string
    if o = null then return "null"; end;
    return $"{o is string}";
  end T;
end U;

public class Program
  public static async method FillA(fs: Array<(int, int) -> int>, os: Array<Object>, k: int): Task<int>
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k - k;
    var n: ((int, int) -> int)? := null;
    var b: ((int, int) -> int)? := null;
    b := n ?? g;
    await Task.Delay(0);
    fs.Add(b!);
    return 0;
  end FillA;

  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      var os: Array<Object> := new Array<Object>();
      t := t + await FillA(fs, os, i);
      for var f in fs do t := t + f(5, 2); end for;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
