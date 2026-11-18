//! case: s28_14_7356
//! doc: spec
//! section: 28.14
//! title: a `scoped` function parameter forwarded to a plain base parameter
//!   is rejected
//! rule: It may not return it, move it into non-`scoped` storage, or store it
//!   in any other object
//! cites: 22.10
//! expect: reject
//! diagnostic: scoped
//! notes: Pins the #8803 fix on its rejecting side: `E`'s `scoped f` goes to
//!   `D`, whose parameter is plain, so the forward does not target a `scoped`
//!   parameter. The accepting twin declares `E`'s parameter plain as well, so
//!   the chain stays call-bounded.
module Conf;

public class B
  private scoped _f: () -> int;
  public constructor(scoped f: () -> int) _f := f; end;
  public method Run(): int => _f();
end;

public class P
  public r: int;
  public constructor(f: () -> int) r := f(); end;
end;
public class D : P
  public constructor(f: () -> int) base(f); end;
end;
public class E : D
  public constructor(scoped f: () -> int) base(f); end;
  public method Run(): int => r;
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
