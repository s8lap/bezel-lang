//! case: s8_5_0902
//! doc: spec
//! section: 8.5
//! title: a class's $"{this}" in an async lambda of its method renders its qualified name, from a record
//! rule: For classes: the type's **qualified name**
//! cites: 18.5, 27.5
//! expect: run
//! output: b Conf.B
//! issue: 8127
module Conf;

public class B
  public constructor()
  end constructor;

  public method Show(): string
    var f := async lambda(): Task<string> => $"b {this}";
    using var t := f();
    return t.Result;
  end Show;
end B;

public record Q(Z: int)
  public method Go(): string
    using var b := new B();
    return b.Show();
  end Go;
end Q;

public class Program
  public static method Main()
    var q := new Q(3);
    Console.WriteLine(q.Go());
  end Main;
end Program;
