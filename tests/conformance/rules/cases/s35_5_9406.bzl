//! case: s35_5_9406
//! doc: spec
//! section: 35.5
//! title: a generic owning match at value and runtime-managed substitutions disposes nothing
//! rule: the payload slot is owned at class and interface substitutions and the marker is erased at
//!   value and runtime-managed substitutions.
//! cites: 35.5, 28.1
//! expect: run
//! output: 1800
module Conf;

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
      s := s + G.Use<string>(Result<string, string>.Ok("xlit"));
      s := s + G.Use<string>(Result<string, string>.Error("elit"));
      s := s + G.Use<int>(Result<int, string>.Ok(i));
      s := s + G.Use<int>(Result<int, string>.Error("eint"));
      s := s + G.Use<Rec>(Result<Rec, string>.Ok(new Rec("rlit", i)));
      s := s + G.Use<Rec>(Result<Rec, string>.Error("erlit"));
      i := i + 1;
    end while;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
