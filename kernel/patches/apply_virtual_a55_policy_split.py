#!/usr/bin/env python3
from pathlib import Path
import sys

path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("drivers/cpufreq/mediatek-cpufreq-hw.c")
reference = Path(__file__).with_name("mediatek-cpufreq-hw.3cluster.c")

if not path.exists():
    raise SystemExit(f"virtual-a55: target driver not found: {path}")
if not reference.exists():
    raise SystemExit(f"virtual-a55: canonical driver not found: {reference}")

path.write_text(reference.read_text())
print(f"virtual-a55: installed peak 3+3+2 policy driver at {path}")
