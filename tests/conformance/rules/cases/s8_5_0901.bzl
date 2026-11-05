//! case: s8_5_0901
//! doc: spec
//! section: 8.5
//! title: a record's $"{this}" in an async lambda of its own method renders the record, called from a class
//! rule: For positional records: a representation of fields, e.g. `"Point(X: 10, Y: 20)"`.
//! cites: 18.5, 27.5
//! expect: run
//! output: r R(K: 7, Q: 9)
//! issue: 8127
module Conf;

public record R(K: int, Q: int)
  public method Show(): string
    var f := async lambda(): Task<string> => $"r {this}";
    using var t := f();
    return t.Result;
  end Show;
end R;

public class Caller
  public constructor()
  end constructor;

  public method Go(): string
    var r := new R(7, 9);
    return r.Show();
  end Go;
end Caller;

public class Program
  public static method Main()
    using var c := new Caller();
    Console.WriteLine(c.Go());
  end Main;
end Program;
