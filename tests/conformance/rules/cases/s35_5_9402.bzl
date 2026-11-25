//! case: s35_5_9402
//! doc: spec
//! section: 35.5
//! title: a generic owning match over an Ok class payload disposes the payload exactly once per call
//! rule: falling off the arm still owning the value auto-drops it with the must-use warning naming
//!   the binding
//! cites: 35.5, 28.1
//! expect: run
//! output: N=200 Made=200 S=200
module Conf;

public class Res
  public static N: int := 0;
  public static Made: int := 0;
  public constructor()
    Res.Made := Res.Made + 1;
  end constructor;
  destructor
    Res.N := Res.N + 1;
  end destructor;
end Res;

public record Rec(S: string, K: int);

public class G
  public static method Use<T>(owned b: Result<T, string>): int
    var k := 0;
    case move b of
      Ok(var v) => k := 1,
      Error(var e) => k := 2
    end case;
    return k;
  end Use;
end G;

public class Program
  public static method Main(): int
    var s := 0;
    var i := 0;
    while i < 200 do
      s := s + G.Use<Res>(Result<Res, string>.Ok(new Res()));
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made} S={s}");
    return 0;
  end Main;
end Program;
