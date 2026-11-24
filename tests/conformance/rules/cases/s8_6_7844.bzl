//! case: s8_6_7844
//! doc: spec
//! section: 8.6
//! title: an Object local compared with = and then returned is the same V
//! rule: a value enters a box by being copied in and leaves it by being read out
//! cites: 8.4, 8.6
//! expect: run
//! output: 200
module Conf;
public record V(X: int) end V;

public class Program
  public static method Make(n: int): Object
    var v := new V(n);
    var o: Object := v;
    if v = o then
      Console.Write("");
    end if;
    if o <> v then
      return new V(-1);
    end if;
    return o;
  end Make;
  public static method Main(): int
    var t := 0;
    for var j in 0 ..< 200 do
      var r := Make(j);
      if r is V then
        t := t + 1;
      end if;
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
