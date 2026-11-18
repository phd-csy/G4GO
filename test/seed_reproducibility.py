"""Compare the single-crystal electron-beam output across Geant4 worker counts."""

import argparse
import csv
import hashlib
import re
import struct
import subprocess
import tempfile
from pathlib import Path
from typing import NamedTuple

import ROOT


class Event(NamedTuple):
    moduleID: int
    Edep: float
    nGenOptPho: int
    nOptPho: int
    ordered_hits_sha256: str


def read_events(directory, threads, count):
    crystals = {}
    sensors = {}
    hit = struct.Struct("<if")
    stem = "run_regression_eminus"
    for worker in range(threads):
        path = directory / stem / f"{stem}_t{worker}.root"
        if not path.is_file():
            raise RuntimeError(f"Missing ROOT output for worker {worker}: {path}")
        with ROOT.TFile.Open(str(path)) as source:
            for row in source.Get("CrystalHit"):
                event = int(row.eventID)
                if event in crystals:
                    raise ValueError(
                        f"Duplicate CrystalHit for event {event} in {path}"
                    )
                crystals[event] = (
                    int(row.moduleID),
                    float(row.Edep),
                    int(row.nGenOptPho),
                    int(row.nOptPho),
                )
            for row in source.Get("SensorHit"):
                event = int(row.eventID)
                ids, times = row.sensorID, row.timeOfFlight
                if event in sensors or not len(ids) or len(ids) != len(times):
                    raise ValueError(f"Invalid SensorHit for event {event} in {path}")
                digest = hashlib.sha256()
                for sensor, time in zip(ids, times):
                    digest.update(hit.pack(sensor, time))
                sensors[event] = (len(ids), digest.hexdigest())

    if crystals.keys() != set(range(count)) or not sensors.keys() <= crystals.keys():
        raise ValueError(f"Incomplete or unexpected event IDs in {directory}")
    events = {}
    empty_hits = (0, hashlib.sha256(b"").hexdigest())
    for event, crystal in crystals.items():
        detected, digest = sensors.get(event, empty_hits)
        if detected != crystal[3]:
            raise ValueError(
                f"SensorHit length differs from nOptPho in {directory}, event {event}"
            )
        events[event] = Event(*crystal, digest)
    return events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=("cpu", "gpu"), required=True)
    parser.add_argument("--build-dir", type=Path, default=Path.cwd())
    parser.add_argument("--events", type=int, default=24)
    parser.add_argument("--output-dir", type=Path, help="New directory for this run")
    args = parser.parse_args()
    if args.events < 6:
        parser.error("--events must be at least 6 so each worker can participate")
    build = args.build_dir.resolve()
    if args.output_dir:
        output = args.output_dir.resolve()
        output.mkdir(parents=True, exist_ok=False)
    else:
        parent = build / "test" / "seed_reproducibility"
        parent.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(prefix=f"{args.backend}-", dir=parent))
    print(f"Results: {output}", flush=True)
    template = (build / "scripts" / "run_regression_eminus.mac").read_text(
        encoding="utf-8"
    )
    text, replacements = re.subn(
        r"^/run/beamOn [0-9]+$", f"/run/beamOn {args.events}", template, flags=re.M
    )
    if replacements != 1:
        raise ValueError(
            "Expected exactly one /run/beamOn command in the electron-beam macro"
        )
    macro = output / "beam.mac"
    macro.write_text(text, encoding="utf-8")

    runs = {}
    with (output / "events.csv").open("w", encoding="utf-8", newline="") as table:
        writer = csv.writer(table)
        writer.writerow(("threads", "eventID", *Event._fields))
        for threads in (1, 2, 6):
            directory = output / f"{threads}t"
            directory.mkdir()
            command = [
                str(build / "g4go"),
                "--seed",
                "1234",
                "--backend",
                args.backend,
                "--threads",
                str(threads),
                str(macro),
            ]
            with (directory / "run.log").open("wb") as log:
                subprocess.run(
                    command,
                    cwd=directory,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=True,
                )
            runs[threads] = read_events(directory, threads, args.events)
            for event, record in sorted(runs[threads].items()):
                writer.writerow((threads, event, *record))

    differences = []
    for threads in (2, 6):
        for event in range(args.events):
            reference, candidate = runs[1][event], runs[threads][event]
            if reference != candidate:
                differences.append(
                    f"FAIL event {event}, 1 vs {threads} workers: {reference} != {candidate}"
                )
                break
    if differences:
        raise SystemExit("\n".join(differences))
    print(
        f"PASS: {args.events} complete events match for 1, 2 and 6 workers ({args.backend}, seed 1234)"
    )


if __name__ == "__main__":
    main()
