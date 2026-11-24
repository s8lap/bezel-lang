# n06-json-order

*Area:* JSON

A nested order: `Order` (class) with `Id: int`, a `Customer` record (`Name`, `Email`), and `Lines`, an array of `Line` records (`Sku: string`, `Qty: int`, `Price: decimal`). Serialize `Order(7, Customer("Ann","ann@x.io"), [Line("A1",2,1.5), Line("B2",1,4)])` to JSON, print it, deserialize it back, and print `lines 2 total 7` and `roundtrip equal: True` (customer equal, every line equal).
