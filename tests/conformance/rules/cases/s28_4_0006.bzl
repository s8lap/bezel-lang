//! case: s28_4_0006
//! doc: spec
//! section: 28.4
//! title: a class may declare at most one destructor
//! rule: A class may declare at most one destructor.
//! expect: reject
//! diagnostic: destructor
module Conf;

public class Widget
  destructor
    Console.WriteLine("first");
  end destructor;

  destructor
    Console.WriteLine("second");
  end destructor;
end Widget;
