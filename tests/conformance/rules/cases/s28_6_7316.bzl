//! case: s28_6_7316
//! doc: spec
//! section: 28.6
//! title: a lambda body `move`-ing its own owned local into an owned field is
//!   admitted
//! rule: Store an owned value instead (a `move` or a fresh production); a
//!   borrow belongs in a non-`owned` field
//! cites: 28.8.1, 28.3
//! expect: run
//! stdout: 2
//! notes: Admitting twin of s28_6_7310 for the `move` spelling: `ys` is a
//!   lambda-local owned binding (a collection literal is a fresh producer),
//!   so `this.saved := move ys` stores an owned value.
module Conf;

class C
  private owned saved: Array<int>?;

  public method Go(): int
    var f := lambda(): int => do
      var ys: Array<int> := [9, 9];
      this.saved := move ys;
      return this.saved!.Count;
    end;
    return f();
  end Go;

  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.Go());
    return 0;
  end Main;
end C;
