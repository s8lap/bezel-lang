_          null        var x       var x: T      Dog       Color.Red     -7
90..100    0..<60      ..<0        31..                                  // range patterns
> 70   < 50   <= 100   >= 0                                              // relational (these four only)
Point(x, y)   Point(_, y)   Point(var x, var y)                          // positional destructuring
Rect(Width = var w, Height = 3)                                          // named destructuring: `=`, not `:=`
Fetched(move response = var r)                                           // marked slot (owning match)
1 | 2 | 3      Red | Blue                                                // alternatives: binding-free
var t when t > 30 => ...                                                   // guard
