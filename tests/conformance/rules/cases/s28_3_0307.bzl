//! case: s28_3_0307
//! doc: spec
//! section: 28.3
//! title: two owned temporaries passed as borrowed arguments to one call are both disposed at statement end
//! rule: Unbound owned temporaries are disposed at their full expression's end, after their uses, in reverse production order
//! cites: 28.3, K8.1
//! expect: run
//! output: 3680 200
//! issue: 7728
module Conf;

public class Res
  public static Count: int := 0;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;

public class H
  public owned C: Res := new Res();
  public S: string;
  public constructor(n: int)
    S := "heap-" + n.ToString() + "-xxxxxxxxxx";
  end constructor;
  public method Name(): string => S;
end H;

public class Make
  public static method MkH(n: int): owned H return new H(n); end MkH;
  public static method Boom(n: int): int
    if n >= 0 then throw new InvalidOperationException("boom"); end;
    return 1;
  end Boom;
end Make;

public class Program
  public static async method Dly(i: int): Task<int> await Task.Delay(1); return 1; end Dly;
  public static method Two(a: string, b: string): int return a.Length + b.Length; end Two;

  public static async method Main(): Task<int>
    var t := 0;

    for var i in 0..<100 do
      t := t + Two(Make.MkH(i).Name(), Make.MkH(i).S) + await Dly(i);
    end;

    Console.WriteLine($"{t} {Res.Count}");
    return 0;
  end Main;
end Program;
