//! case: l5_9005
//! doc: stdlib
//! section: 5
//! title: eager terminals accept generic-class, other-class and interface-receiver method groups
//! rule: are call-bounded as their unmarked signatures state — they invoke the callback during the call and
//!   retain nothing.
//! expect: run
//! output: 454400
//! notes: Harvested from the #9012 review (p15). Four method-group spellings into Sum over [5,6]:
//!   Helper<string>.S = (5+100)+(6+100) = 211; Other.S = 1005 + 1006 = 2011; isel.P and impl.P =
//!   (5+7)+(6+7) = 25 each. 211 + 2011 + 25 + 25 = 2272 per iteration x 200 = 454400.
module Conf;
public interface ISel
  method P(i: int): int;
end ISel;
public class Impl: ISel
  public method P(i: int): int => i + 7;
end Impl;
public class Helper<T>
  public static method S(i: int): int => i + 100;
end Helper;
public class Other
  public static method S(i: int): int => i + 1000;
end Other;
public class Program
  public static method Main(): int
    var t := 0;
    var impl := new Impl();
    var isel: ISel := impl;
    for var i in 0 ..< 200 do
      var a := new Array<int>();
      a.Add(5); a.Add(6);
      t := t + a.Sum(Helper<string>.S);
      t := t + a.Sum(Other.S);
      t := t + a.Sum(isel.P);
      t := t + a.Sum(impl.P);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
