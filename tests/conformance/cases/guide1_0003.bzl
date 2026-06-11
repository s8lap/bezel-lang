method Leak(): View
  using var c := new Connection("...");
  return c.View();        // ERROR: borrow of c, but c is disposed at block end
end Leak;
