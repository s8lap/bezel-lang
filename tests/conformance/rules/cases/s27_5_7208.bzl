//! case: s27_5_7208
//! doc: spec
//! section: 27.5
//! title: a lambda stored before a continue keeps that iteration's loop
//!   variable
//! rule: each iteration produces a fresh binding. A lambda captured inside
//!   the body sees that iteration's value.
//! cites: 27.5, 21.4
//! expect: run
//! output: 22700
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
      var g := new G();
      for var i in 0..<10 do
        g.Cb := lambda(x: int) => x + i;
        if i mod 2 = 0 then
          continue;
        end if;
        t := t + 1;
      end for;
      t := t + g.Call(r);
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
