public class Registry  // declaration sketch; method bodies elided
  public method Append(item: Widget);         // borrows; caller keeps ownership
  public method Append(owned item: Widget);   // takes; the caller's binding is consumed
end Registry;

r.Append(w);            // borrow — the plain overload; w untouched
r.Append(move w);       // transfer — the owned overload; w moved-from (§22.3)
r.Append(new Widget()); // fresh producer: an owned production — the owned overload
