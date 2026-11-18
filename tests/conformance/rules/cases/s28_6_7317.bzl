//! case: s28_6_7317
//! doc: spec
//! section: 28.6
//! title: a lambda body storing the result of an `owned`-returning call into
//!   an owned field is admitted
//! rule: Store an owned value instead (a `move` or a fresh production); a
//!   borrow belongs in a non-`owned` field
//! cites: 28.3, 28.8
//! expect: run
//! stdout: 1
//! notes: Admitting twin of s28_6_7310 for a call result: `Make()` returns
//!   `owned Array<int>`, a fresh producer, so the store is marker-free and
//!   admitted.
module Conf;

class C
  private owned saved: Array<int>?;

  public static method Make(): owned Array<int>
    var a := new Array<int>();
    a.Add(1);
    return a;
  end Make;

  public method Go(): int
    var f := lambda(): int => do this.saved := Make(); return this.saved!.Count; end;
    return f();
  end Go;

  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.Go());
    return 0;
  end Main;
end C;
