//! case: s27_5_7205
//! doc: spec
//! section: 27.5
//! title: a lambda stored from a for-loop body sees that iteration's loop
//!   variable
//! rule: each iteration produces a fresh binding. A lambda captured inside
//!   the body sees that iteration's value.
//! cites: 27.5, 21.4
//! expect: run
//! output: 120900
module Conf;

public class G
  public Cb: ((int) -> int)?;

  public method Call(x: int): int
    var f := Cb;
    if f is not null then
      return f(x);
    end if;
    return -1;
  end Call;
end G;

public class Program
  public static method Main()
    var t := 0;
    for var r in 0..<200 do
      var g0 := new G();
      var g1 := new G();
      var g2 := new G();
      for var i in 0..<3 do
        if i = 0 then
          g0.Cb := lambda(x: int) => x + i * 100 + r;
        elif i = 1 then
          g1.Cb := lambda(x: int) => x + i * 100 + r;
        else
          g2.Cb := lambda(x: int) => x + i * 100 + r;
        end if;
      end for;
      t := t + g0.Call(1) + g1.Call(2) + g2.Call(3);
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
