//! case: s35_5_9503
//! doc: spec
//! section: 35.5
//! title: a generic owning match arm may read its payload and then move it once
//! rule: In an owning match, an `owned`-payload binding (`var r` above) is an owned local of the arm
//!   body's scope
//! cites: 35.5, 28.1, 28.3
//! expect: run
//! output: N=200 Made=200 S=600
//! notes: Spec-decided control of the #9221 fix (pick 21cc495b2), green on both arms (the base ran these
//!   exactly, its defects being sanitizer-visible only): a case-arm binding of a bare type parameter
//!   T was not an owned (owning match) or borrowed (borrowing match) local, as its class-typed twin's
//!   is. Variant the enclosing type's parameter T. Drops are counted by the destructor: every Res is
//!   disposed exactly once. Ok arm: Peek 1, then + Sink 0 = 1; Error arm 2; 600.
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

public class H<T>
  public static method Sink(owned x: T): int
    return 0;
  end Sink;
  public static method Peek(x: T): int
    return 1;
  end Peek;
  public static method Use(owned b: Result<T, string>): int
    var k := 0;
    case move b of
      Ok(var v) => do k := H<T>.Peek(v); k := k + H<T>.Sink(move v); end,
      Error(var e) => k := 2
    end case;
    return k;
  end Use;
end H;

public class Program
  public static method Main(): int
    var s := 0;
    var i := 0;
    while i < 200 do
      s := s + H<Res>.Use(Result<Res, string>.Ok(new Res()));
      s := s + H<Res>.Use(Result<Res, string>.Error("e"));
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made} S={s}");
    return 0;
  end Main;
end Program;
