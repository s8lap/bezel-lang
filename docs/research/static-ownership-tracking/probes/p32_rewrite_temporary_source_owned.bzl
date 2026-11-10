module Probe;
// REWRITE of the temporary-source shape (corpus case s22_6_2312), form 2: the holder OWNS its items (an owned field and an owned constructor parameter), so the literal is transferred, not borrowed. EXPECT: compiles; prints 15; no report line. Under A the class holds no borrow.
public class Row
  public owned Items: Array<int>;
  public constructor(owned a: Array<int>) Items := a; end constructor;
end Row;
public class Program
  public static method Main(): int
    using var r1 := new Row([1, 2]);
    using var r2 := new Row([3, 4, 5]);
    var t := 0;
    for var v in r1.Items do t := t + v; end;
    for var v in r2.Items do t := t + v; end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
