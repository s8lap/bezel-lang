//! case: s28_2_6715
//! doc: spec
//! section: 28.2
//! title: a generic owned parameter read after its explicit Dispose is an error
//! rule: A use of a local variable — a read, a borrow taken from it, a member access, an argument position, a consume — after an explicit `Dispose()`
//! cites: 28.3, 14.2
//! expect: reject
//! diagnostic: dominat
module Conf;

public class Res
  private _n: int;
  public constructor(n: int)
    _n := n;
  end constructor;
  public method Ping(): int => _n;
end Res;

public class Program
  public static method G<T>(owned x: T): int where T: IDisposable
    x.Dispose();
    var y := x;
    return 1;
  end G;
  public static method Main()
    Console.WriteLine(Program.G<Res>(new Res(1)));
  end Main;
end Program;
