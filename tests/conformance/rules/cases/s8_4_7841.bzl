//! case: s8_4_7841
//! doc: spec
//! section: 8.4
//! title: and, or and is over = between a record local and an Object local holding a V
//! rule: a boxed `T` argument compares by it on the box's payload, any other
//!   argument, `null` included, unequal
//! cites: 8.4, 17.2, 9.2
//! expect: run
//! output: 800
module Conf;
public record V(X: int) end V;

public class Program
  public static method Once(n: int): int
    var v := new V(n);
    var o: Object := v;
    var p: Object? := null;
    var c := 0;
    if (v = o) and (o = v) then
      c := c + 1;
    end if;
    if (v <> o) or (o = v) then
      c := c + 1;
    end if;
    if (p = null) and (v = o) then
      c := c + 1;
    end if;
    var q: Object? := o;
    if (q is V) and (v = o) then
      c := c + 1;
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
