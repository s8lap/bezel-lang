//! case: s19_3_0079
//! doc: spec
//! section: 19.3
//! title: an interface binding over a T = int? substitution is rejected at the instantiation site
//! rule: "a type pattern with binding is rejected at the instantiation site whatever the static answer, as `as` is" (§19.3, v0.340, *Type patterns* paragraph): a value substitution (here `int?`) has no box, and a binding to a reference type would form one.
//! cites: 12.7, 3.6
//! expect: reject
//! diagnostic: reference-typed binding
module Conf;

public interface IShape
  method Id(): int;
end IShape;

public class Program2
  public static method G<T>(v: T): string
    return (case v of var s: IShape => "shape", _ => "other");
  end G;
end Program2;

public class Program
  public static method Main()
    Console.WriteLine(Program2.G<int?>(42));
  end Main;
end Program;
