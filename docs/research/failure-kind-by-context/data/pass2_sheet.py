#!/usr/bin/env python3
"""Prints the blank sheet for the second classification pass: every (task, operation) pair
of scenarios.tsv in a fixed shuffled order, with the task and the operation only (no
condition, no feature columns, no first-pass label, no justification).
Usage: pass2_sheet.py scenarios.tsv"""
import csv, random, sys
rows = list(csv.DictReader(open(sys.argv[1], encoding="utf-8"), delimiter="\t"))
random.Random(20261110).shuffle(rows)
for r in rows:
    print(f"{r['id']}\t{r['task']}\t{r['operation']}")
