# n02-import

*Area:* errors (v0.359)

Import three order files written to a scratch directory, plus one path that does not exist. Each line is `sku,qty,price`; a malformed line is a *decoded* error returned as `Result<Order, string>` and counted, not thrown. Per file print `a.csv: 2 ok, 1 bad (line 2: bad qty 'x')` style lines (first bad line only). A missing file is answered by catching `FileNotFoundException` by name at the call and prints `missing.csv: not found`. Data: `a.csv` = `A1,2,1.50` / `A2,x,3.00` / `A3,1,4.00`; `b.csv` = `B1,1,10.00` / `B2,2,5.00` / `B3,1,14.50`; `c.csv` = `C1;2;3`. Finally print `total 3 files, 5 orders, value 41.50`. Expected: `a.csv: 2 ok, 1 bad (line 2: bad qty 'x')`, `b.csv: 3 ok, 0 bad`, `c.csv: 0 ok, 1 bad (line 1: expected 3 fields)`, `missing.csv: not found`, `total 3 files, 5 orders, value 41.50`.
