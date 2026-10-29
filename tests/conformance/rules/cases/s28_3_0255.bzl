//! case: s28_3_0255
//! doc: spec
//! section: 28.3
//! title: A rebind after the pin does not make the pin borrow the consumed owner
//! rule: Only a live local borrow of the consumed binding's current value freezes the consume (spec 28.3.9). Here the alias is rebound, or the pin sits in a different branch or after the rebind, so the pin borrows another owner (or none) and the consume of the first owner is admitted; the program runs with each destructor once and each use before its owner's disposal.
//! cites: 28.3, 28.10
//! expect: run
//! output: D:2
//!   D:1
//!   U:3
//!   D:3
//!   D:4
//!   D:2
//!   D:1
//!   U:3
//!   D:3
//!   D:4
//!   END
module Conf;

public interface IUse
  method Use();
end IUse;

public record Hold(Item: Q);
public record Pt(X: int);
public record Nm(S: string);
public enum Kd
  K1, K2;
end Kd;
public choice Ch
  Has(q: Q),
  Nada;
end Ch;

public sealed class Q: IUse
  private _id: int;
  public constructor(id: int)
    _id := id;
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
  public method Use()
    Console.WriteLine($"U:{_id}");
  end Use;
end Q;

public sealed class R: IUse
  private _id: int;
  public owned f: Q;
  public num: int;
  public nm: string := "nm";
  public constructor(id: int, owned q: Q)
    _id := id;
    f := q;
    num := id;
  end constructor;
  destructor
    Console.WriteLine($"D:{_id}");
  end destructor;
  public method Use()
    Console.WriteLine($"U:{_id}");
  end Use;
  public method Self(): R => this;
  public method MaybeSelf(): R? => this;
  public method Wrap(): Hold => new Hold(f);
  public method GetPt(): Pt => new Pt(_id);
  public method GetNm(): Nm => new Nm(nm);
  public method GetKd(): Kd => Kd.K2;
  public method GetCh(): Ch => Ch.Has(f);
  public method MaybeNum(): int? => _id;
  public method Dur(): TimeSpan => TimeSpan.FromSeconds(1.0);
  public static method Take(owned r: R)
    using var k := move r;
  end Take;
  public static method TakeQ(owned q: Q)
    using var k := move q;
  end TakeQ;
  public static method Boom(k: int)
    if k = 0 then
      throw new InvalidOperationException("boom");
    end if;
  end Boom;
  public static method SinkT<T>(owned x: T)
    using var k := move x;
  end SinkT;
  public static method Body(k: int)
    var a := new R(2, new Q(1));
    var c := new R(3, new Q(4));
    var b := c;
    using var p := b;
    b := a;
    R.Take(move a);
    p.Use();
  end Body;
end R;

public class Program
  public static method Main(): int
    for var it in 0..<1 do
      R.Body(0);
      R.Body(1);
    end;
    Console.WriteLine("END");
    return 0;
  end Main;
end Program;
