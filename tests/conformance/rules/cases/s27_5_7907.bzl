//! case: s27_5_7907
//! doc: spec
//! section: 27.5
//! title: a parameter captured in a nested branch and again at the top level reads correctly
//!   in both orders
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 225600
//! notes: Fix pin for a closure-cell leak: the corpus runner has no leak leg, so this asserts
//!   the program output; leak freedom is covered by the gtest rows.
module P;
public class Program
  public static method Apply(f: () -> int): int => f();
  public static method M(p: int): int
    var r := 0;
    if p mod 2 = 0 then
      r := Apply(lambda() => p + 1);
    end if;
    r := r + Apply(lambda() => p + 2);
    return r + p;
  end M;
  public static method N(p: int): int
    var r := Apply(lambda() => p + 2);
    if p mod 2 = 0 then
      r := r + Apply(lambda() => p + 1);
    end if;
    return r + p;
  end N;
  public static method Main(): int
    var t := 0;
    for var j in 0..<300 do t := t + Program.M(j) + Program.N(j); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
