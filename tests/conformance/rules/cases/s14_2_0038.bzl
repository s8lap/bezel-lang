//! case: s14_2_0038
//! doc: spec
//! section: 14.2
//! title: a class conforming to a static-member interface satisfies its recurrence constraint
//! rule: An interface that declares or inherits **static** members (§11.8) is
//!   conformed to by no interface under this rule ... so `interface IThing:
//!   IZero<IThing>`, over an `IZero<T>` declared without §11.8's recurrence
//!   constraint, is a well-formed declaration that satisfies `where T:
//!   IZero<T>` at no instantiation site — `Gen.Make<IThing>()` is a compile
//!   error naming the constraint
//! cites: 14.2, 11.8
//! expect: accept
module Conf;

public interface IZero<T>
  static method Zero(): owned T;
end IZero;

public class Thing: IZero<Thing>
  public constructor()
  end constructor;

  public static method Zero(): owned Thing => new Thing();
end Thing;

public class Gen
  public static method Make<T>(): owned T where T: IZero<T> => T.Zero();
end Gen;

public class User
  public static method Run(): int
    using var t := Gen.Make<Thing>();
    return 1;
  end Run;
end User;
