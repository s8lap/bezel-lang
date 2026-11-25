//! case: s19_3_9502
//! doc: spec
//! section: 19.3
//! title: a borrowing match over a value-constrained generic copies the payload at int and record
//! rule: For value-type scrutinees, the binding is a copy by the standard value-type rules
//! cites: 19.3, 28.1
//! expect: run
//! output: q=1600
//! notes: Spec-decided control of the #9221 fix (pick 21cc495b2), where T: value: the class-assumed
//!   predicate excludes it. Ok arms: three Peeks = 3, Error arm 2: (3 + 3 + 2) * 200 = 1600.
module Conf;

public record P(A: int);

public class G
  public static method Peek<T>(x: T): int
    return 1;
  end Peek;

  public static method Use<T>(b: Result<T, string>): int where T: value
    var k := 0;
    case b of
      Ok(var v) => do var a := v; var c := v; k := G.Peek<T>(a) + G.Peek<T>(c) + G.Peek<T>(v); end,
      Error(var e) => k := 2
    end case;
    return k;
  end Use;
end G;

public class Program
  public static method Main(): int
    var q := 0;
    var i := 0;
    while i < 200 do
      q := q + G.Use<int>(Result<int, string>.Ok(5));
      q := q + G.Use<P>(Result<P, string>.Ok(new P(1)));
      q := q + G.Use<int>(Result<int, string>.Error("e"));
      i := i + 1;
    end while;
    Console.WriteLine($"q={q}");
    return 0;
  end Main;
end Program;
