//! case: s21_7_0005
//! doc: spec
//! section: 21.7
//! title: a total record statement-case grants totality with no trailing return
//! rule: The statement-`case` totality grant is keyed to §19.6's
//!   closed-value-space registry, and that registry now carries the
//!   **record** bullet: a statement-`case` over a non-nullable record
//!   scrutinee whose unguarded arm is an all-slots-irrefutable destructure
//!   satisfies condition (i), so a callable may end with it and needs no
//!   trailing `return`.
//! cites: 21.7, 19.6, 9.6
//! expect: run
//! stdout: 7
//! issue: 4372
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Sum(p: Point): int
    case p of
      Point(var x, var y) => return x + y;
    end case;
  end Sum;

  public static method Main()
    Console.WriteLine(Sum(new Point(3, 4)).ToString());
  end Main;
end Program;
