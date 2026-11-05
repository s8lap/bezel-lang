//! case: s26_1_0901
//! doc: spec
//! section: 26.1
//! title: in an extension block `this` inside an async lambda is the receiver
//! rule: `this` refers to the receiver.
//! cites: 8.5, 27.5
//! expect: run
//! output: x Conf.B 42
//! issue: 8127
module Conf;

public class B
  public N: int := 42;

  public constructor()
  end constructor;
end B;

public extension B
  public method Show(): string
    var f := async lambda(): Task<string> => $"x {this} {this.N}";
    using var t := f();
    return t.Result;
  end Show;
end extension;

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
