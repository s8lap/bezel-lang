private owned _items: Array<int> := new();          // class-typed field taking a fresh
private owned _config: Config := new(verbose := true);   //   value is `owned` (§28.6)
var xs: Array<int> := new();
