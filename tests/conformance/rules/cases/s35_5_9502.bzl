//! case: s35_5_9502
//! doc: spec
//! section: 35.5
//! title: a generic owning match arm may move its payload on one branch and read it on the other
//! rule: standard rules apply to it on every arm-exit edge
//! cites: 35.5, 28.1, 28.3
//! expect: run
//! output: N=200 Made=200 S=400
//! notes: Spec-decided control of the #9221 fix (pick 21cc495b2), green on both arms (the base ran these
//!   exactly, its defects being sanitizer-visible only): a case-arm binding of a bare type parameter
//!   T was not an owned (owning match) or borrowed (borrowing match) local, as its class-typed twin's
//!   is. Variant `where T: IThing` (a user interface). Drops are counted by the destructor: every Res
//!   is disposed exactly once. Ok arm: k = 0 so the move branch runs and Sink returns 0 (Sink
//!   disposes it); Error arm 2: 400.
module Conf;

public interface IThing
  method Tag(): int;
end IThing;

public class Res: IThing
  public static N: int := 0;
  public static Made: int := 0;
  public constructor()
    Res.Made := Res.Made + 1;
  end constructor;
  destructor
    Res.N := Res.N + 1;
  end destructor;
  public method Tag(): int
    return 1;
  end Tag;
end Res;

public class G
  public static method Sink<T>(owned x: T): int where T: IThing
    return 0;
  end Sink;
  public static method Peek<T>(x: T): int where T: IThing
    return 1;
  end Peek;
  public static method Use<T>(owned b: Result<T, string>): int where T: IThing
    var k := 0;
    case move b of
      Ok(var v) => do if k = 0 then k := G.Sink<T>(move v); else k := G.Peek<T>(v); end if; end,
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
      s := s + G.Use<Res>(Result<Res, string>.Error("e"));
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made} S={s}");
    return 0;
  end Main;
end Program;
