//! case: s28_3_0231
//! doc: spec
//! section: 28.3
//! title: A borrowed iterable parameter returned under an unmarked return type is rejected via a conditional
//! rule: An unmarked iterable-family return type is mint-announcing, and returning a borrow under it is rejected (spec 28.3 announcement table, row 2); the declaration of Id is rejected whatever its use sites.
//! cites: 28.3, 29.3
//! expect: reject
//! diagnostic: borrow
//! issue: 7679
module Conf;
public class H
  public static fins: int := 0;
  public method GenS(): owned IEnumerable<int>
    try yield return 1; yield return 2;
    finally H.fins := H.fins + 1; end;
  end GenS;
  public method Id(e: IEnumerable<int>): IEnumerable<int>
    return e;
  end Id;
end H;
public class Program
  public static method Main(): int
    var sum := 0; var i := 0;
    while i < 50 do
      using var h := new H();
      using var e := h.GenS();
      var c := i > -1;
      for var x in (if c then h.Id(e) else null)! do sum := sum + x; end;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " fins=" + H.fins.ToString());
    return 0;
  end Main;
end Program;
