//! case: s23_3_8601
//! doc: spec
//! section: 23.3
//! title: a setter body that reads its borrowed value parameter in a loop, a call and a lambda is admitted
//! rule: The setter's implicit `value` parameter has no modifier by default and takes a borrow of the property's declared type.
//! cites: 23.3, 28.3.10
//! expect: run
//! stdout: 0abcdef9
//! notes: Harvested from the #9073 review (probe t1_value_forms, trimmed: the owned-temporary argument is replaced by a using local declared before h, and the two property stores go through a helper taking plain parameters, because a borrow of a scope-bound local stored through a property is rejected by §28.3's property-store bullet, a second error ground). Apply runs the Items setter (sum of 1+2 stored in total, then Write(copy.Count) of an empty Array<int>(3) prints 0 (Count = 0, stdlib Array<T>(capacity))) and then the B setter. Name (called from Main after Apply): parts = 6 > 3 so _name := abcdef, printed. B setter: Check(value) true so total := 4, the lambda returns value.n + 1 = 5, total = 9, WriteLine prints 9. Output 0abcdef9.
module Conf;
import System;
public class Box
  public n: int := 0;
  public constructor(n: int) this.n := n; end constructor;
end Box;
public class H
  private owned _items: Array<int>;
  private _name: string := "";
  private _b: Box? := null;
  public total: int := 0;
  public constructor() _items := new Array<int>(2); end constructor;
  private method Check(b: Box): bool return b.n >= 0; end Check;
  public property Items: Array<int>
    get => _items;
    set => do
      var sum := 0;
      for var x in value do sum := sum + x; end for;
      total := sum;
      var copy := new Array<int>(3);
      Console.Write(copy.Count);
    end;
  end;
  public property Name: string
    get => _name;
    set => do
      var parts := value.Length;
      if parts > 3 then _name := value; else _name := "short"; end if;
    end;
  end;
  public property B: Box
    get => _b!;
    set => do
      if Check(value) then total := value.n; end if;
      var f := lambda(): int => value.n + 1;
      total := total + f();
    end;
  end;
end H;
public static class Program
  public static method Apply(h: H, items: Array<int>, b: Box)
    h.Items := items;
    h.B := b;
  end Apply;
  public static method Main(args: Array<string>): int
    using var l := new Array<int>(2); l.Add(1); l.Add(2);
    using var bx := new Box(4);
    var h := new H();
    Program.Apply(h, l, bx);
    h.Name := "abcdef"; Console.Write(h.Name);
    Console.WriteLine(h.total);
    return 0;
  end;
end Program;
