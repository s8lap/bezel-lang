//! case: s22_6_0100
//! doc: spec
//! section: 22.6
//! title: no candidate is applicable when the generic overload's closed parameter does not match the argument
//! rule: "with no applicable candidate at all, the no-candidate error" ([§22.6]). The generic F<T> needs its second parameter to be IBox<string>, and IntBox implements only IBox<int>; the non-generic overload needs a string first argument, and the literal 5 is an int. Neither candidate is applicable.
//! cites: 14.5, 12.9
//! expect: reject
//! diagnostic: no overload of `F` is applicable
//! issue: 6747
module Conf;
public interface IBox<T>
  method Get(): T;
end IBox;
public class Box<T>: IBox<T>
  private _v: T;
  public constructor(v: T)
    _v := v;
  end constructor;
  public method Get(): T => _v;
end Box;
public class IntBox: Box<int>
  public constructor()
    base(42);
  end constructor;
end IntBox;
public static class S
  public static method F<T>(x: T, b: IBox<string>): int => 3;
  public static method F(x: string, b: IBox<int>): int => 2;
end S;
public class Program
  public static method Main(): int
    using var a := new IntBox();
    Console.WriteLine(S.F(5, a));
    return 0;
  end Main;
end Program;
