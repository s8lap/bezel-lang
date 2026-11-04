//! case: s22_6_0208
//! doc: spec
//! section: 22.6
//! title: members of this, and this itself at a parameter of its own type, are admitted at the same calls
//! rule: **identity**: every type converts to itself;
//! cites: 5.1, 12.3
//! expect: run
//! output: 10
//!   0
//! issue: 8130
module Conf;

public class C
  public F: int := -5;

  public method Show(): string
    return $"{this.F}";
  end Show;

  public method Take(o: C): bool
    return true;
  end Take;

  public method Go(): int
    var a := Math.Abs(this.F);
    var s := "abc".StartsWith(this.Show());
    var t := Convert.ToInt32(this.F.ToString());
    var u := this.Take(this);
    var f := lambda() => Math.Abs(this.F);
    return a + f();
  end Go;

  public method Nested(): int
    var f := lambda() => "abc".StartsWith(this.Show());
    if f() then
      return 1;
    end if;
    return 0;
  end Nested;
end C;

public class Program
  public static method Main()
    using var c := new C();
    Console.WriteLine(c.Go());
    Console.WriteLine(c.Nested());
  end Main;
end Program;
