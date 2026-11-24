//! case: s8_6_7842
//! doc: spec
//! section: 8.6
//! title: an Object local compared with = and then stored in a field still holds the V
//! rule: a value enters a box by being copied in and leaves it by being read out
//! cites: 8.4, 8.6
//! expect: run
//! output: 200
module Conf;
public record V(X: int) end V;

public class Holder
  public Kept: Object? := null;
end Holder;
public static class Sink
  public static readonly H: Holder := new Holder();
end Sink;

public class Program
  public static method Once(n: int)
    var v := new V(n);
    var o: Object := v;
    if v = o then
      Sink.H.Kept := o;
    end if;
    if o = v then
      Console.Write("");
    end if;
  end Once;
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      Once(j);
      var k := Sink.H.Kept;
      if k is V then
        t := t + 1;
      end if;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
