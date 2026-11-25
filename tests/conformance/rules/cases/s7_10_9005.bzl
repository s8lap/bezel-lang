//! case: s7_10_9005
//! doc: spec
//! section: 7.10
//! title: a generic class method returning () -> T? over the class parameter is usable at int and string
//! rule: `T?` always means "the nullable form of T," regardless of constraint.
//! cites: 14.1, 7.2, 27.5
//! expect: run
//! output: 19900
//!   lit
//! notes: Harvested from the #9120 review (b4). The nullable type parameter is the generic CLASS parameter,
//!   not a method parameter. Trace: sum of i over 0..199 = 19900; the string instantiation prints the
//!   captured literal.
module Conf;
public class Holder<T>
  private _v: T;
  public constructor(v: T) this._v := v; end constructor;
  public method Getter(): () -> T?
    var v := this._v;
    return lambda(): T? => v;
  end Getter;
end Holder;
public class Program
  public static method Main(): int
    var tot := 0;
    var i := 0;
    while i < 200 do
      var h := new Holder<int>(i);
      var f := h.Getter();
      tot := tot + (f() ?? -1);
      i := i + 1;
    end;
    Console.WriteLine(tot);
    var hs := new Holder<string>("lit");
    var fs := hs.Getter();
    Console.WriteLine(fs() ?? "nul");
    return 0;
  end Main;
end Program;
