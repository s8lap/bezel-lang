//! case: s35_5_9404
//! doc: spec
//! section: 35.5
//! title: a generic method forwarding an owned result with move and nesting a match disposes it once
//! rule: falling off the arm still owning the value auto-drops it with the must-use warning naming
//!   the binding
//! cites: 35.5, 28.1
//! expect: run
//! output: N=400 Made=400 S=7600
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

public class H
  public static method Use2<T>(owned b: Result<T, string>): int
    return G.Use<T>(move b) + 10;
  end Use2;
  public static method Use3<T>(owned b: Result<T, string>): int
    var k := 0;
    case move b of
      Ok(var v) => k := G.Use<T>(Result<T, string>.Error("inner")),
      Error(var e) => k := 2
    end case;
    return k;
  end Use3;
end H;

public class Program
  public static method Main(): int
    var s := 0;
    var i := 0;
    while i < 200 do
      s := s + H.Use2<Res>(Result<Res, string>.Ok(new Res()));
      s := s + H.Use2<Res>(Result<Res, string>.Error("e"));
      s := s + H.Use2<string>(Result<string, string>.Ok("qlit"));
      s := s + H.Use3<Res>(Result<Res, string>.Ok(new Res()));
      s := s + H.Use3<Rec>(Result<Rec, string>.Ok(new Rec("rlit", i)));
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made} S={s}");
    return 0;
  end Main;
end Program;
