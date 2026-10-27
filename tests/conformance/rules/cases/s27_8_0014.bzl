//! case: s27_8_0014
//! doc: spec
//! section: 27.8
//! title: an alias of an alias used as a parameter type is the aliased record's type
//! rule: "A type alias is a pure synonym — it does not create a new type"
//!   (§27.8, v0.340), and an alias of an alias is still that synonym; when a
//!   test succeeds the carrier "is flow-narrowed to `T` within the
//!   controlled block" (§12.7, v0.340) and a `with` "result has the same
//!   static type as the source" (§9.5, v0.340). So a parameter spelled
//!   through an alias of an alias is narrowed and copied like the record
//!   itself.
//! cites: 27.8, 9.5, 12.7
//! expect: run
//! output: 44
//!   alpha
//!   44
//!   beta
module Conf;

public record P(Sku: string, Stock: int);
type PAlias := P;
type PAlias2 := PAlias;

public class Program
  public static method Bump(p: PAlias?): P
    if p is P then
      var r: P := p with(Stock := 44);
      return r;
    end;
    return new P("none", 0);
  end Bump;

  public static method Bump2(p: PAlias2?): P
    if p is P then
      var r: P := p with(Stock := 44);
      return r;
    end;
    return new P("none", 0);
  end Bump2;

  public static method Main()
    var r := Bump(new P("alpha", 5));
    Console.WriteLine(r.Stock);
    Console.WriteLine(r.Sku);
    var r2 := Bump2(new P("beta", 5));
    Console.WriteLine(r2.Stock);
    Console.WriteLine(r2.Sku);
  end Main;
end Program;
