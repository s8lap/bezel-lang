//! case: s28_3_4306
//! doc: spec
//! section: 28.3
//! title: a plain var enumerator moved into another var is disposed once by the binding that discharges it
//! rule: a marked binding or assignment transfer — `var y := move x;`
//! cites: 28.3.1, 29.7, 28.1
//! expect: run
//! output: True
//!   True
//!   fin
//!   True
//!   fin
//!   end
module Conf;

class G
  public static method Gen(): IEnumerable<int>
    try
      yield return 1;
      yield return 2;
    finally
      Console.WriteLine("fin");
    end try;
  end Gen;
end G;

class Y
  public static method Sink(owned e: IEnumerator<int>)
    Console.WriteLine(e.MoveNext());
  end Sink;
end Y;

public class Program
  public static method Main()
    for var i in 0..<2 do
      var e := G.Gen().GetEnumerator();
      Console.WriteLine(e.MoveNext());
      var m := move e;
      if i mod 2 = 0 then
        Y.Sink(move m);
      else
        m.Dispose();
      end if;
    end for;
    Console.WriteLine("end");
  end Main;
end Program;
