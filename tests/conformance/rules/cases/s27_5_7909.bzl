//! case: s27_5_7909
//! doc: spec
//! section: 27.5
//! title: an async method capturing its parameter on several paths, one returning early, sums
//!   correctly
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9, 5.1
//! expect: run
//! stdout: 2870
//! notes: Fix pin for a closure-cell leak: the corpus runner has no leak leg, so this asserts
//!   the program output; leak freedom is covered by the gtest rows.
module P;
public class Program
  public static method Apply(f: () -> int): int => f();
  public static async method A(p: int): Task<int>
    await Task.Delay(1);
    var t := 0;
    if p mod 2 = 0 then
      t := Apply(lambda() => p + 1);
    end if;
    if p mod 3 = 0 then
      return t + p;
    end if;
    await Task.Delay(1);
    do
      t := t + Apply(lambda() => p + 5);
    end;
    return t;
  end A;
  public static async method Main(): Task<int>
    var acc := 0;
    for var j in 0..<60 do acc := acc + await Program.A(j); end;
    Console.WriteLine(acc.ToString());
    return 0;
  end Main;
end Program;
