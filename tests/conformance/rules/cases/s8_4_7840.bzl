//! case: s8_4_7840
//! doc: spec
//! section: 8.4
//! title: = between a record local and an Object local holding a V decides by payload after reassignment
//! rule: a boxed `T` argument compares by it on the box's payload, any other
//!   argument, `null` included, unequal
//! cites: 8.4, 8.6, 9.2
//! expect: run
//! output: 20200
module Conf;
public record V(X: int) end V;

public class Program
  public static method Once(n: int): int
    var v := new V(n);
    var o: Object := v;
    var c := 0;
    if v = o then
      c := c + 1;
    end if;
    o := new V(n + 1);
    if v = o then
      c := c + 10;
    end if;
    if o = new V(n + 1) then
      c := c + 100;
    end if;
    o := v;
    if o <> v then
      c := c + 1000;
    end if;
    return c;
  end Once;
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      t := t + Once(j);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
