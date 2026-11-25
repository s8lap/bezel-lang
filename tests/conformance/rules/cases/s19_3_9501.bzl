//! case: s19_3_9501
//! doc: spec
//! section: 19.3
//! title: a generic borrowing case expression arm binds the payload as a borrow whose source is the scrutinee
//! rule: the bound name is a borrow whose source is the scrutinee's source
//! cites: 19.3, 28.1, 35.5
//! expect: run
//! output: N=200 Made=201 S=400
//! notes: Spec-decided control of the #9221 fix (pick 21cc495b2). The match is over b without `move`, so
//!   the payload binding is a borrow of b (read twice through Peek: 2 per iteration, 400); b is an
//!   owned parameter that is disposed at the callee's end, once per Res: 200 of the 201 made (the
//!   201st is dd, alive at the print).
module Conf;

public interface IThing
  method Tag(): int;
end IThing;

public class Res: IThing
  public static N: int := 0;
  public static Made: int := 0;
  public constructor()
    Res.Made := Res.Made + 1;
  end constructor;
  destructor
    Res.N := Res.N + 1;
  end destructor;
  public method Tag(): int
    return 1;
  end Tag;
end Res;

public class G
  public static method Peek<T>(x: T): int
    return 1;
  end Peek;

  public static method Use<T>(owned b: Result<T, string>, d: T): int
    var got := (case b of Ok(var v) => v, Error(var e) => d);
    return G.Peek<T>(got);
  end Use;
end G;

public class Program
  public static method Main(): int
    var s := 0;
    var i := 0;
    using var dd := new Res();
    while i < 200 do
      s := s + G.Use<Res>(Result<Res, string>.Ok(new Res()), dd);
      s := s + G.Use<Res>(Result<Res, string>.Error("e"), dd);
      i := i + 1;
    end while;
    Console.WriteLine($"N={Res.N} Made={Res.Made} S={s}");
    return 0;
  end Main;
end Program;
