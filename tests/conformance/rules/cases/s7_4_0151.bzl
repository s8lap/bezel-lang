//! case: s7_4_0151
//! doc: spec
//! section: 7.4
//! title: a mutable nullable field narrowed by a null test is still rejected at Math.Min
//! rule: It never narrows mutable fields, properties, or `ref` parameters.
//! cites: 7.1, 2.2
//! expect: reject
//! diagnostic: nullable
//! issue: 7660
//! notes: Twin: the readonly-field row s7_4_0152, which differs by the one keyword readonly.
module Conf;

public class Box
  private _v: int?;

  public constructor(v: int?)
    _v := v;
  end constructor;

  public method Get(): int
    if _v <> null then
      return Math.Min(_v, 5);
    end if;
    return 0;
  end Get;
end Box;

public class Program
  public static method Main()
    using var b := new Box(3);
    Console.WriteLine(b.Get());
  end Main;
end Program;
