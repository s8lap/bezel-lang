//! case: s28_3_7_7644
//! doc: spec
//! section: 28.3.7
//! title: a nested lambda may not mark `move` on a store into an owned field
//! rule: an owned-field or `set(owned)` store spelled `sink._f := move x;`
//! cites: 28.3.7, 17.8
//! expect: reject
//! diagnostic: redundant
module Conf;
import System;
public class C
  public owned keep: Array<int>?;
  public owned other: Array<int>?;
  public method Go(): int
    var f := lambda(): int => do
      var g := lambda(): int => do
        var ys := new Array<int>(); ys.Add(5);
        this.keep := move ys;
        return 1;
      end;
      return g();
    end;
    return f();
  end Go;
  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.Go());
    return 0;
  end Main;
end C;
