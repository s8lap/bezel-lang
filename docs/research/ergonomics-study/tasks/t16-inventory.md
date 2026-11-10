# t16-inventory

*Area:* collections

A stock dictionary item -> quantity starts at {bolt: 10, nut: 5}. Apply transactions (item, delta): (bolt, -3), (nut, -7), (washer, 4), (bolt, -8). Unknown items are created at 0 first. After all, print items in name order `bolt -1`, `nut -2`, `washer 4`, then `negative: bolt, nut`.
