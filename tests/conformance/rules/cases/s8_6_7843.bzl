//! case: s8_6_7843
//! doc: spec
//! section: 8.6
//! title: a lambda comparing a captured Object local holding a V with = and returning it reads the box intact
//! rule: a value enters a box by being copied in and leaves it by being read out
//! cites: 8.4, 8.6, 27.5
//! expect: run
//! output: 600
module Conf;
public record V(X: int) end V;

public class Program
  public static method Mk(n: int): () -> bool
    var v := new V(n);
    var o: Object := v;
    if v = o then
      Console.Write("");
    end if;
    return lambda() => o = v;
  end Mk;
  public static method Mk3(n: int): () -> Object
    var v := new V(n);
    var o: Object := v;
    if v <> o then
      Console.Write("");
    end if;
    return lambda() => o;
  end Mk3;
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      var f := Mk(j);
      if f() then
        t := t + 1;
      end if;
      var h := Mk3(j);
      var r := h();
      if r is V then
        t := t + 1;
      end if;
      var f2 := Mk(j);
      if f2() then
        t := t + 1;
      end if;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
