//! case: s28_14_7357
//! doc: spec
//! section: 28.14
//! title: a `scoped` function parameter passed to another object's
//!   constructor is rejected
//! rule: It may not return it, move it into non-`scoped` storage, or store it
//!   in any other object
//! cites: 22.10
//! expect: reject
//! diagnostic: scoped
//! notes: Pins the #8803 fix: `E` builds a second `B` in `_o := new B(f)`,
//!   which is a call on a different bounding object than the one under
//!   construction.
module Conf;

public class B
  private scoped _f: () -> int;
  public constructor(scoped f: () -> int) _f := f; end;
  public method Run(): int => _f();
end;

public class E : B
  private owned _o: B;
  public constructor(scoped f: () -> int) base(f); _o := new B(f); end;
end;

public static class Program
  public static method Main(): int
    var i := 0; var tot := 0;
    while i < 200 do
      var s := "hi" + i.ToString();
      using var k := new E(lambda(): int => s.Length);
      tot := tot + k.Run();
      i := i + 1;
    end;
    Console.Write(tot);
    return 0;
  end;
end;
