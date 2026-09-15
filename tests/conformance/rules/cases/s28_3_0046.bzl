//! case: s28_3_0046
//! doc: spec
//! section: 28.3
//! title: a case scrutinee's unbound owned temporary survives the selected arm
//! rule: A `case` scrutinee extends the disposal boundary to the whole `case`
//!   construct, so pattern bindings survive the selected arm.
//! cites: C.3
//! expect: run
//! output: w
//!   disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("disposed");
  end destructor;

  public method Label(): string => "w";
end Widget;

public class Factory
  public constructor()
  end constructor;

  public method Make(): owned Widget => new Widget();
end Factory;

public class Program
  public static method Main()
    var f := new Factory();
    case f.Make() of
      var x: Widget => Console.WriteLine(x.Label())
    end case;
  end Main;
end Program;
