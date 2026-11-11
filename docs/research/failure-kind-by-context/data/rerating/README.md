# An independent blind re-rating of the scenario table

*Added 2026-11-11 with proposal [0214](../../../../proposals/0214-narrowed-failure-marker.md). §2 of the
note asked for an independent relabelling before a proposal cites its rates; this is that
relabelling, on a sample. Evidence labels are the note's.*

## Method

- **The sheet.** `blind.tsv` is the scenario table of `../scenarios.tsv` with every judgement
  removed: id, domain, task, operation and condition only (no operand origin, fallback, scope,
  label or justification).
- **The sample.** `sample.py` draws it, seed `20261111`: for each condition with five or more
  rows, 30% of its rows (at least three); one row for each smaller condition. 100 pairs
  (`sample.tsv`). MEASURED: re-running `sample.py` reproduces `sample.tsv` byte for byte.
- **The rater.** A separate critic lane, which had not seen the note's labels, rated each
  sampled pair on the brief's three bands — (a) handle at the call, (b) report at a boundary,
  (c) stop — from the sheet alone (`mine.tsv`), and then split its band-(c) rows into *fatal
  here, code right* (F) and *a bug* (D) (`mine_fd.txt`).
- **The comparison.** `cmp.py` maps the note's first-pass labels to the three bands (L→a, B→b,
  F and D→c), computes agreement and Cohen's κ, prints the confusion matrix and every
  disagreement, and computes κ on bug-or-not (the re-rater's D against the note's D).
  `RESULTS.txt` is its output.

The scripts are the critic's, with their paths made relative to this directory and the
bug-or-not computation added at the end of `cmp.py`.

## Results

MEASURED (`cmp.py` → `RESULTS.txt`).

| | |
|---|---|
| pairs | 100 |
| three-band agreement | 77% |
| three-band κ | **0.66** |
| bug-or-not agreement | 96% |
| bug-or-not κ | **0.89** |

Confusion matrix, three bands (rows the re-rater, columns the note):

| | note a | note b | note c |
|---|---|---|---|
| **re-rater a** | 20 | 2 | 1 |
| **re-rater b** | 13 | 22 | 6 |
| **re-rater c** | 1 | 0 | 35 |

Most disagreements are a against b (15 of 23): whether the "unit of work" is the call or the
boundary above it, the boundary the note's §2 already calls soft. Bug-or-not, the fact a class
could be asked to carry, is the stable judgement.

**The headlines on the sample** (MEASURED from the same files and `../conditions.tsv`; a
row's group is its condition's v0.357 class):

| Group | Rows | Bugs, re-rater | Bugs, note |
|---|---|---|---|
| environment and peer classes (I/O, network, end of stream, time-out, channel) | 29 | 0 | 1 |
| `FormatException`, `SerializationException`, `ParseRangeException` | 22 | 5 | 7 |
| the classes v0.357 called defects | 35 | 17 | 16 |
| other (cancellation, allocation, the domain join) | 14 | 1 | 1 |

Every headline of the note's §1 reproduces in direction: the environment and peer classes are
almost never bugs, the data classes sometimes are, and the classes v0.357 called defects are
bugs in about half of their rows. On the full table (the note's labels, 272 pairs) the same
three groups are a bug in 3 of 106, 8 of 66 and 37 of 89 rows.

## Limits

- One re-rater, on a sample of 100; the confusion matrix has small cells.
- The re-rater, like the first rater, is a model, so the two share whatever a model's reading
  of a one-line task description shares. Agreement is evidence that the labels are not one
  rater's quirk, not that they are right.
- The F/D split was made only within band (c), so a row the re-rater put in (a) or (b) counts
  as not a bug.
