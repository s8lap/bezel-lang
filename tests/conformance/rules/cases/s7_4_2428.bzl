//! case: s7_4_2428
//! doc: spec
//! section: 7.4
//! title: A null test on a mutable nullable field does not narrow it at `in`
//! rule: It never narrows mutable fields, properties, or `ref` parameters.
//! cites: 7.4, 17.5
//! expect: reject
//! diagnostic: narrowing
//! notes: Single ground: l is a mutable field, which 7.4 never narrows, so `x in l`
//!   is a call on a nullable value despite the null test. Replacing the guarded
//!   `x in l` return by the field-copy idiom `var u := l; if u <> null then
//!   return x in u; end if;` compiles. Not red on base by verdict: the saved
//!   pre-fix compiler also rejected it, with the 'not yet implemented' text, so
//!   only the diagnostic needle (reported, never gated) differs.
module P;

public class H
  public l: IList<int>?;
  public constructor() l := null; end constructor;
  public method Has(x: int): bool
    if l <> null then return x in l; end if;
    return false;
  end Has;
end H;
class C
  public static method Main(): int
    var h := new H();
    Console.WriteLine(h.Has(7));
    return 0;
  end Main;
end C;
