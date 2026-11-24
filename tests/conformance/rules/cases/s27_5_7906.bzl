//! case: s27_5_7906
//! doc: spec
//! section: 27.5
//! title: a parameter captured only on a branch that is sometimes not taken reads correctly
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 59800
//! notes: Fix pin for a closure-cell leak: the corpus runner has no leak leg, so this asserts
//!   the program output; leak freedom is covered by the gtest rows.
module P;
public class Program
  public static method Apply(f: () -> int): int => f();
  public static method M(p: int): int
    var r := 0;
    if p mod 3 = 0 then
      r := Apply(lambda() => p + 1);
    end if;
    return p + r;
  end M;
  public static method Main(): int
    var t := 0;
    for var j in 0..<300 do t := t + Program.M(j); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
