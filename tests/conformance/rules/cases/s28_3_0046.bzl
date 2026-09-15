//! case: s28_3_0046
//! doc: spec
//! section: 28.3
//! title: a case scrutinee's unbound owned temporary survives the selected arm
//! rule: A case scrutinee extends the disposal boundary to the whole case construct,
//!   keeping pattern bindings alive through the selected arm.
//! expect: run
//! stdout: w
module Conf;

public class Widget
  destructor
    Console.WriteLine("disposed");
  end destructor;

  public method Label(): string => "w";
end Widget;

public class Factory
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
