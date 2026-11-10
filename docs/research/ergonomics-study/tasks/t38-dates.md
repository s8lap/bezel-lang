# t38-dates

*Area:* formatting

Parse `2026-02-14` and `2026-11-10` as dates, print the days between (`269 days`), the first date plus 30 days formatted `yyyy-MM-dd` (`2026-03-16`), the weekday names of both (`Saturday`, `Monday`), and a `TimeSpan` of 90061 seconds rendered as `1d 01:01:01`.

*Erratum (found while running):* 2026-11-10 is a Tuesday, not a Monday; the program's output is right.
