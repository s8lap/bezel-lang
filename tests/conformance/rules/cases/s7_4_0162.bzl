//! case: s7_4_0162
//! doc: spec
//! section: 7.4
//! title: a nullable function-typed mutable field tested against null is not invocable in the guarded
//!   block
//! rule: Accessing a nullable *mutable* field without first copying it to a local is a compile error when
//!   the access requires non-null.
//! cites: 7.4
//! expect: reject
//! diagnostic: narrowing
//! issue: 8099
module Conf;

public class D
  private _f: ((int) -> int)? := lambda(x: int) => x + 11;

  public method Go(): int
    if _f <> null then
      return _f(1);
    end if;
    return 0;
  end Go;
end D;

public class Program
  public static method Main()
    using var d := new D();
    Console.WriteLine(d.Go());
  end Main;
end Program;
