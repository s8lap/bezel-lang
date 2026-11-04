//! case: s7_4_0163
//! doc: spec
//! section: 7.4
//! title: copying a mutable nullable function field to a local and narrowing the copy admits the
//!   invocation
//! rule: Local-variable narrowing applies to the copy, which no other thread can observe.
//! cites: 7.4
//! expect: run
//! output: 12
module Conf;

public class D
  private _f: ((int) -> int)? := lambda(x: int) => x + 11;

  public method Go(): int
    var u := _f;
    if u <> null then
      return u(1);
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
