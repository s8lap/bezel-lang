//! case: s29_8_7443
//! doc: spec
//! section: 29.8
//! title: consumption on both branches of an if then again after it is rejected
//! rule: a second consumption is a compile error where the flow analysis can see it and raises
//!   `InvalidOperationException` where it is reachable only at run time
//! cites: 29.8, 29.11
//! expect: reject
//! diagnostic: was already consumed
module Conf;
public class Box
  public static live: int := 0;
  public V: int;
  public constructor(v: int) V := v; Box.live := Box.live + 1; end constructor;
  destructor Box.live := Box.live - 1; end destructor;
end Box;
public class Gen
  public K: int := 0;
  public method Items(owned b: Box): IEnumerable<int>
    for var k in 0..<3 do yield return k + b.V + K; end;
  end Items;
end Gen;
public class Program
  public static method GenM(owned b: Box): IEnumerable<int>
    using var r := move b;
    for var k in 0..<3 do yield return k + r.V; end;
  end GenM;
  public static method GenE(owned b: Box): IEnumerable<int>
    for var k in 0..<3 do yield return k + b.V; end;
  end GenE;
  public static method GenP(b: Box): IEnumerable<int>
    for var k in 0..<3 do yield return k + b.V; end;
  end GenP;
  public static method GenT(owned a: Box, owned b: Box): IEnumerable<int>
    using var r := move a;
    for var k in 0..<3 do yield return k + r.V + b.V; end;
  end GenT;
  public static method GenF(owned b: Box, n: int): IEnumerable<int>
    yield return b.V;
    if n > 0 then throw new ArgumentException("boom"); end;
    yield return b.V + 1;
  end GenF;
  public static method GenC(owned b: Box): IEnumerable<Box>
    for var k in 0..<3 do yield return new Box(k + b.V); end;
  end GenC;
  public static method Use(src: IEnumerable<int>): int
    var t := 0;
    for var x in src do t := t + x; end;
    return t;
  end Use;
  public static method Show(o: Object): int
    return 1;
  end Show;
  public static method Ident<T>(x: T): int
    return 2;
  end Ident;
  public static method Use2(a: IEnumerable<int>, b: IEnumerable<int>): int
    return Use(a) + Use(b);
  end Use2;
  public static method Twice(src: IEnumerable<int>): int
    var t := Use(src);
    try
      t := t + Use(src);
    catch
      InvalidOperationException => do t := t + 1000; end,
    end try;
    return t;
  end Twice;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do
      using var src := GenE(new Box(1));
      if i mod 2 = 0 then t := t + Use(src); else t := t + Use(src); end;
      t := t + Use(src);
    end;
    Console.WriteLine($"{t} live={Box.live}");
    return 0;
  end Main;
end Program;
