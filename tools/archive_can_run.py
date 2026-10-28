#!/usr/bin/env python3
"""Write reproducible provenance and output hashes for one OMNeT++ run."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_tree_hash(paths: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda p: p.as_posix()):
        relative = path.relative_to(ROOT).as_posix().encode()
        digest.update(relative + b"\0")
        digest.update(bytes.fromhex(sha256(path)))
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--config", required=True)
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--observer", choices=("on", "off"), default="on")
    parser.add_argument("--omnet-root", type=Path,
                        default=Path(sys.argv[0]).resolve().parents[1] / ".tools/omnetpp-6.0.2")
    args = parser.parse_args()
    run_dir = args.run_dir.resolve()
    trace = run_dir / "events.jsonl"
    binary = ROOT / "omnet/src/canlab"
    dbc = ROOT / "data/carla_multi_ecu.dbc"
    manifest = ROOT / "integration/omnet/ecu_manifest.json"
    version_file = args.omnet_root / "Version"
    required = [trace, binary, dbc, manifest, version_file]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        print("cannot archive run; missing files: " + ", ".join(missing), file=sys.stderr)
        return 2

    source_paths = [ROOT / "omnet/Makefile", ROOT / "scripts/build_omnet.sh"]
    source_paths += [p for base in (ROOT / "omnet/src", ROOT / "omnet/ned", ROOT / "omnet/simulations")
                     for p in base.rglob("*") if p.is_file() and p.suffix in {".cc", ".cpp", ".h", ".msg", ".ned", ".ini"}]
    validation_paths = [ROOT / "scripts/test_phase1.sh", ROOT / "scripts/run_omnet.sh",
                        ROOT / "tests/test_analyze_can_experiment.py"]
    validation_paths += [p for base in (ROOT / "omnet/tests", ROOT / "tools")
                         for p in base.rglob("*") if p.is_file() and p.suffix in {".cc", ".py", ".json", ".csv"}]
    compiler = subprocess.run(["g++", "--version"], check=False, capture_output=True, text=True)
    output_files = sorted(p for p in run_dir.rglob("*") if p.is_file() and p.name != "run_metadata.json")
    record = {
        "format": "canlab-run-metadata-v1",
        "config": args.config,
        "seed": args.seed,
        "observer": args.observer,
        "dbc": {"path": "data/carla_multi_ecu.dbc", "sha256": sha256(dbc)},
        "runtime_manifest": {"path": "integration/omnet/ecu_manifest.json", "sha256": sha256(manifest)},
        "build": {
            "binary": "omnet/src/canlab",
            "binary_sha256": sha256(binary),
            "source_tree_sha256": source_tree_hash(source_paths),
            "validation_suite_sha256": source_tree_hash(validation_paths),
        },
        "toolchain": {
            "omnetpp_version": version_file.read_text().strip(),
            "compiler": compiler.stdout.splitlines()[0] if compiler.stdout else "unknown",
            "platform": platform.platform(),
        },
        "outputs": [
            {"path": path.relative_to(run_dir).as_posix(), "size_bytes": path.stat().st_size,
             "sha256": sha256(path)} for path in output_files
        ],
    }
    target = run_dir / "run_metadata.json"
    target.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    print(f"wrote {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
