//! case: s35_5_9403
//! doc: spec
//! section: 35.5
//! title: a generic owning match with returns in every arm disposes a class payload once
//! rule: falling off the arm still owning the value auto-drops it with the must-use warning naming
//!   the binding
//! cites: 35.5, 28.1
//! expect: run
//! output: N=200 Made=200 S=6000
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
    case move b of
      Ok(var v) => do
        return 7;
      end,
      Error(var e) => do
        return 9;
      end
    end case;
    return 0;
  end Use;
end G;

public class Program
  public static method Main(): int
    var s := 0;
    var i := 0;
    while i < 200 do
      s := s + G.Use<Res>(Result<Res, string>.Ok(new Res()));
      s := s + G.Use<Res>(Result<Res, string>.Error("badlit"));
      s := s + G.Use<string>(Result<string, string>.Ok("xlit"));
      s := s + G.Use<Rec>(Result<Rec, string>.Ok(new Rec("rlit", i)));
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made} S={s}");
    return 0;
  end Main;
end Program;
