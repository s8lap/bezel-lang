//! case: s9_5_0003
//! doc: spec
//! section: 9.5
//! title: with over a class-typed source is rejected
//! rule: The source expression must be record-typed; a with expression whose
//!   source is class-typed is a compile error naming this rule, the retired
//!   object-initializer reading.
//! cites: 22.11, 28.3
//! expect: reject
//! diagnostic: record-typed
module Conf;

public class Widget
  public property Label: string;

  public constructor(label: string)
    Label := label;
  end constructor;
end Widget;

public class Program
  public static method Renamed(w: Widget): string => (w with(Label := "x")).Label;
end Program;
