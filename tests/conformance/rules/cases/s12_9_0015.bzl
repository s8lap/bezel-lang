//! case: s12_9_0015
//! doc: spec
//! section: 12.9
//! title: a concrete generic class is invariant in its type argument
//! rule: "Classes, records, and concrete generic types are always invariant" ([§12.9]). Array<ArgumentException> does not convert to Array<Exception>, although ArgumentException derives from Exception.
//! cites: 14.3
//! expect: reject
//! diagnostic: not implicitly convertible to the target type
module Conf;
public class Program
  public static method Main(): int
    using var a := new Array<ArgumentException>();
    var e: Array<Exception> := a;
    return 0;
  end Main;
end Program;
