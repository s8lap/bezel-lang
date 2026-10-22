//! case: s12_9_0016
//! doc: spec
//! section: 12.9
//! title: a nullable element view does not reverse to the non-nullable interface
//! rule: "The relation a variant conversion asks of the two arguments is a reference conversion" ([§12.9]), whose nullable forms are listed as "the nullable view of a reference type (`string` to `string?`)" ([§12.9]). The view runs from non-null to nullable only: IEnumerable<ArgumentException?> does not convert to IEnumerable<Exception>, because ArgumentException? to Exception is not a view of that kind.
//! cites: 5.1, 7.1
//! expect: reject
//! diagnostic: not implicitly convertible to the target type
module Conf;
public class Program
  public static method Main(): int
    using var a := new Array<ArgumentException?>();
    var e: IEnumerable<Exception> := a;
    return 0;
  end Main;
end Program;
