# t07-money

*Area:* data modelling

A `Money` value (amount as decimal, currency code). Adding two Money of the same currency yields their sum; different currencies throw an `InvalidOperationException` with message `currency mismatch: USD vs EUR`. Print `12.50 USD + 7.25 USD = 19.75 USD`, then a copy of a price with only the amount changed (`price with amount 3.00 -> 3.00 USD`), then the caught mismatch message. Equality: `new Money(1.0m,"USD") = new Money(1.00m,"USD")` prints `equal: True`.
