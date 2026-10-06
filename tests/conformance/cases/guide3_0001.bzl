var max    := (if a > b then a else b);                       // two-way

var fee    := (if                                            // condition ladder
  amount > 10_000m then 0m,
  isPremium        then 1m,
  else               2.50m);

var label  := (case outcome of                                 // shape dispatch
  Fetched(var body) => $"got {body.Length} bytes",
  NotModified       => "cached",
  Failed(var code)  => $"error {code}");
