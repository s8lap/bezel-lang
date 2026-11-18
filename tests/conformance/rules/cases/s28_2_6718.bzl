//! case: s28_2_6718
//! doc: spec
//! section: 28.2
//! title: a slice of a span taken before the kill is a holder, so reading it after the kill is an error
//! rule: a use of the holder — a read or member access through it, passing it, re-borrowing it, invoking the closure, enumerating or disposing the frame — is the same compile error
//! cites: 28.3, 9.7
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
  public static method Run(): int
    var xs: Array<int> := [1, 2, 3];
    var s := xs.AsSpan();
    var t := s.Slice(1);
    xs.Dispose();
    return t[0];
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;
