var max    := (if a > b then a else b);                       // two-way

var fee    := (when                                            // condition ladder
  amount > 10_000m => 0m,
  isPremium        => 1m,
  _                => 2.50m);

var label  := (case outcome of                                 // shape dispatch
  Fetched(var body) => $"got {body.Length} bytes",
  NotModified       => "cached",
  Failed(var code)  => $"error {code}");
