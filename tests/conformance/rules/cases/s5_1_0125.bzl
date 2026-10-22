//! case: s5_1_0125
//! doc: spec
//! section: 5.1
//! title: a covariant interface does not convert from a base element type to a derived one
//! rule: "A value of one type implicitly converts to another when no programmer-visible information is lost" ([§5.1]). The reference conversions admitted are "class to base class, transitively, up to `Object`" ([§5.1]); IEnumerable<Exception> to IEnumerable<ArgumentException> would narrow the element type, which needs an explicit cast.
//! cites: 12.9
//! expect: reject
//! diagnostic: not implicitly convertible to the target type
module Conf;
public class Program
  public static method Main(): int
    using var a := new Array<Exception>();
    var e: IEnumerable<ArgumentException> := a;
    return 0;
  end Main;
end Program;
