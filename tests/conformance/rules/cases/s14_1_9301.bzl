//! case: s14_1_9301
//! doc: spec
//! section: 14.1
//! title: a nullable generic-record field, return and ?? operand carry the payload at each instantiation
//! rule: **Instantiation and its site.** An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 14.1, 7.2
//! expect: run
//! output: -1
//!   21
//!   5
//!   1031
//!   41
//!   21
//!   q
//!   1031
//!   41
//! notes: Harvested from the #9049 review (p5, first half; the second half is s14_1_9302). `Holder<V>` keeps a
//!   field `_slot: Entry<V>?`. Trace at V = int: Peek on the unset field is -1; after Set(5) the slot holds
//!   Entry(5, 21), so Peek is 21 and Val(9) returns the payload 5; UseMake(1): Make(.., true) is
//!   Entry(1, 31) so the first term is 31, Make(.., false) is null so the second is 1000: 1031; Coalesce:
//!   the null left operand takes the right one, Entry(1, 41), so 41. At V = string: Set("q") then Peek is 21,
//!   Val("d") is "q", UseMake is 1031, Coalesce is 41.
module Conf;
public record Entry<V>(Value: V, Expires: int);
public class Holder<V>
  private _slot: Entry<V>?;
  public method Set(v: V)
    _slot := new Entry<V>(v, 21);
  end Set;
  public method Peek(): int
    if _slot is not null then return _slot!.Expires; end if;
    return -1;
  end Peek;
  public method Val(def: V): V
    var e := _slot;
    return (if e = null then def else e!.Value);
  end Val;
  public method Make(v: V, flag: bool): Entry<V>?
    if flag then return new Entry<V>(v, 31); end if;
    return null;
  end Make;
  public method UseMake(v: V): int
    var a := Make(v, true);
    var b := Make(v, false);
    return (if a = null then -1 else a!.Expires) + (if b = null then 1000 else 0);
  end UseMake;
  public method Coalesce(v: V): int
    var a: Entry<V>? := null;
    var d := new Entry<V>(v, 41);
    var c := a ?? d;
    return c.Expires;
  end Coalesce;
end Holder;
public class Program
  public static method Main(): int
    using var h := new Holder<int>();
    Console.WriteLine(h.Peek());
    h.Set(5);
    Console.WriteLine(h.Peek());
    Console.WriteLine(h.Val(9));
    Console.WriteLine(h.UseMake(1));
    Console.WriteLine(h.Coalesce(1));
    using var s := new Holder<string>();
    s.Set("q");
    Console.WriteLine(s.Peek());
    Console.WriteLine(s.Val("d"));
    Console.WriteLine(s.UseMake("z"));
    Console.WriteLine(s.Coalesce("z"));
    return 0;
  end Main;
end Program;
