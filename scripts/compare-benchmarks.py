#!/usr/bin/env python3
"""Check corpus output equivalence and report timing changes, without timing gates."""

import argparse
import json
from pathlib import Path
from statistics import median


def compare(before, after):
    def documents(report):
        return {doc["sha256"]: doc for doc in report["documents"]}

    old, new = documents(before), documents(after)
    if old.keys() != new.keys():
        raise ValueError("Corpus hashes differ")
    for sha, doc in old.items():
        updated = new[sha]
        if doc["pages"] != updated["pages"] or doc["matches"] != updated["matches"]:
            raise ValueError(f"Page/search results changed: {doc['file']}")
        def renders(document):
            return {(r["page"], r["clip"], r["theme"]): r for r in document["renders"]}
        previous, current = renders(doc), renders(updated)
        if previous.keys() != current.keys():
            raise ValueError(f"Render workload changed: {doc['file']}")
        for key, render in previous.items():
            if render["pixels_sha256"] != current[key]["pixels_sha256"]:
                raise ValueError(f"Pixels changed: {doc['file']} {key}")
        print(f"{doc['file']}: {len(current)} renders and search results identical")
        for theme in ("light", "comfort"):
            a = median(r["p50_ms"] for r in previous.values() if r["theme"] == theme)
            b = median(r["p50_ms"] for r in current.values() if r["theme"] == theme)
            print(f"  {theme}: median of per-case medians {a:.3f} -> {b:.3f} ms")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()
    try:
        compare(json.loads(args.before.read_text()), json.loads(args.after.read_text()))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
