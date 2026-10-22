//! case: s21_10_0023
//! doc: spec
//! section: 21.10
//! title: a local record that implements an interface is returned boxed through that interface
//! rule: "that implements an interface may be" "such a box is genuinely self-contained"
//! cites: 8.6, 9.6
//! expect: run
//! output: 16
//! issue: 6910
module Conf;

public interface IShape
  method Area(): int;
end IShape;

public class Program
  public static method Make(): owned IShape
    record Sq(S: int): IShape
      public method Area(): int
        return S * S;
      end Area;
    end Sq;
    return new Sq(4);
  end Make;

  public static method Main()
    Console.WriteLine(Make().Area());
  end Main;
end Program;
