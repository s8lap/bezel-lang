//! case: s28_3_0248
//! doc: spec
//! section: 28.3
//! title: A using var pin of a record carrying a string does not freeze the consumed owner
//! rule: A using var over a value read (an int, a record or enum value, a string or a TimeSpan produced from the owner) is a pin of a value that holds no borrow of the owner's storage, so consuming or reassigning the owner afterwards has no live local borrow to freeze (spec 28.3.9: a value-typed read from the owner is no borrow, spec 28.10). The pinned value stays readable and the owner is disposed once.
//! cites: 28.3, 28.10
//! expect: run
//! output: D:2
//!   D:1
//!   nm
//!   D:2
//!   D:1
//!   nm
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
    using var p := a.GetNm();
    R.Take(move a);
    Console.WriteLine(p.S);
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
