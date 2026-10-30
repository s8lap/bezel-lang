//! case: s35_5_0034
//! doc: spec
//! section: 35.5
//! title: a case over o! of an owning choice does not move the scrutinee local
//! rule: the construct never moves from the scrutinee
//! cites: 17.8, 19.3, 15.3
//! expect: run
//! output: 100
module Conf;

public class Res
  public static Count: int := 0;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;

public choice OC
  Text(owned r: Res),
  Empty;
end OC;

public class Program
  public static method T()
    var o: OC? := OC.Text(new Res());
    var n := 0;
    case o! of
      Text(var r) => n := 1,
      Empty => n := 2
    end case;
    var x := o;
  end T;

  public static method Main(): int
    for var i in 0..<100 do
      T();
    end;
    Console.WriteLine($"{Res.Count}");
    return 0;
  end Main;
end Program;
