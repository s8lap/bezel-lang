//! case: s11_2_0164
//! doc: spec
//! section: 11.2
//! title: a value of an extending interface exposes the extended interface's member
//! rule: Language spec §11.2 (v0.345): "An interface may extend other interfaces … Multiple-
//!   interface inheritance is permitted" — and the inherited surface is the extending interface's
//!   own: "`Add`, `Clear`, `Remove`, and the rest of the mutating surface in the example above are
//!   inherited from `ICollection<T>`."
//! cites: 11.2, 11.3
//! expect: run
//! stdout: hi
module Conf;

public interface IThing
  method Speak(): string;
end IThing;

public interface ISub: IThing
end ISub;

public class Program
  public static method Main()
    var x: ISub := new Thing();
    Console.WriteLine(x.Speak());
  end Main;

  public class Thing: ISub
    public method Speak(): string
      return "hi";
    end Speak;
  end Thing;
end Program;
