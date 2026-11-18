//! case: s28_6_7315
//! doc: spec
//! section: 28.6
//! title: a lambda body storing a fresh `new Array<int>()` into an owned
//!   field is admitted
//! rule: Store an owned value instead (a `move` or a fresh production); a
//!   borrow belongs in a non-`owned` field
//! cites: 28.5, 28.8
//! expect: run
//! stdout: 2
//! notes: Admitting twin of s28_6_7310 to s28_6_7313: the same lambda-body
//!   store into an owned field, with a fresh production on the right. Each
//!   call displaces the previous value, which is disposed (28.6 assignment
//!   rule), and returns the new array's Count of 1.
module Conf;

class C
  private owned saved: Array<int>?;

  public method Go(): int
    var f := lambda(k: int): int => do
      this.saved := new Array<int>();
      this.saved!.Add(k);
      return this.saved!.Count;
    end;
    return f(7) + f(8);
  end Go;

  public static method Main(): int
    using var c := new C();
    Console.WriteLine(c.Go());
    return 0;
  end Main;
end C;
