//! case: s28_8_1_4203
//! doc: spec
//! section: 28.8.1
//! title: a nullable IEnumerator field refilled after a bare take is taken and nulled again
//! rule: A nullable location a bare take leaves holding `null` is an ordinary null field: its holder's cascade disposes nothing through it, and a later read reads `null`
//! cites: 28.8.1, 29.3, stdlib 4.12
//! expect: run
//! output: True
//!   True
//!   end
//! issue: 7977
module Conf;

class G
  public static method It(): IEnumerator<int>
    yield return 5;
    yield return 6;
  end It;
end G;

class H
  public owned En: IEnumerator<int>? := null;

  public method Fill()
    En := G.It();
  end Fill;
end H;

public class Program
  public static method Main()
    using var h := new H();
    for var i in 0..<4 do
      h.Fill();
      using var p := move h.En;
      if i mod 2 = 1 then
        using var q := move h.En;
        Console.WriteLine(q is null);
      end if;
    end for;
    Console.WriteLine("end");
  end Main;
end Program;
