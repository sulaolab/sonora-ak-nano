"""Extract complete PWMscope lines without treating UART as scope evidence."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    lines = args.log.read_text(encoding="utf-8-sig", errors="replace").splitlines()
    if any("[source=uart]" in line and "[partial" not in line and
           f" PWMhal id={args.tag} " in line for line in lines):
        raise SystemExit("HAL PWM telemetry uses PWMhal/PWMhalL fields; this analyzer supports only direct PWMscope logs")
    statuses, samples = [], []
    active_tag = None
    for line in lines:
        # Raw hex previews and partial UART fragments cannot supply full fields.
        if "[source=uart]" not in line or "[partial" in line:
            continue
        if " PWMscope id=" in line:
            fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
            active_tag = fields.get("id")
            required = ("ms", "ready", "sleep", "pll2", "clk5", "pg", "dma",
                        "cnt", "half", "done", "err", "fail")
            if active_tag == args.tag and all(k in fields for k in required):
                statuses.append({"time": line[:12], **{k: int(fields[k]) for k in required}})
        elif " PWMscope pcmL_q31=" in line and active_tag == args.tag:
            fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
            required = ("pcmL_q31", "pcm_blk", "per", "wr", "overlap", "miss")
            if not all(k in fields for k in required) or not all(k in fields for k in ("duty", "tdm", "rtf", "source")):
                continue
            duty = fields["duty"].split("/")
            tdm = fields["tdm"].split("/")
            harm = fields["rtf"].split("/")
            if len(duty) != 2 or len(tdm) != 2 or len(harm) != 3:
                continue
            samples.append({"time": line[:12], **{k: int(fields[k]) for k in required},
                            "duty_min": int(duty[0]), "duty_max": int(duty[1]),
                            "tdm_running": int(tdm[0]), "tdm_blk": int(tdm[1]),
                            "rx": int(harm[0]), "tx": int(harm[1]), "frame": int(harm[2]),
                            "source": fields["source"], "mapping": fields.get("map", "processed_float")})
    if not statuses or not samples:
        raise SystemExit("No complete status/data lines for the requested scope ID")
    args.out.mkdir(parents=True, exist_ok=True)
    for name, rows in (("status", statuses), ("samples", samples)):
        with (args.out / f"{name}.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    neutral = [r for r in samples if r["source"] == "neutral"]
    settled_neutral = [r for r in neutral if 2 * r["duty_min"] == r["per"] == 2 * r["duty_max"]]
    summary = {
        "scope_tag": args.tag,
        "log_file": args.log.name,
        "log_sha256": hashlib.sha256(args.log.read_bytes()).hexdigest(),
        "status_records": len(statuses), "sample_records": len(samples),
        "first_status": statuses[0], "last_status": statuses[-1],
        "intentional_or_unexpected_stopped_records": sum(not r["ready"] for r in statuses),
        "sleep_nonlow_records": sum(r["sleep"] != 0 for r in statuses),
        "maximum_dma_errors": max(r["err"] for r in statuses),
        "maximum_watchdog_failures": max(r["fail"] for r in statuses),
        "maximum_producer_overlap": max(r["overlap"] for r in samples),
        "maximum_tdm_deadline_misses": max(r["miss"] for r in samples),
        "maximum_rx_tx_frame_harm": {k: max(r[k] for r in samples) for k in ("rx", "tx", "frame")},
        "neutral_records": len(neutral), "exact_half_duty_records": len(settled_neutral),
        "audio_records": sum(r["source"] == "audio" for r in samples),
        "mapping_modes": sorted({r["mapping"] for r in samples}),
        "physical_scope_evidence": "absent; UART does not measure frequency, polarity or nSLEEP voltage",
        "timebase": "ms is the CPU FRC-derived tick; it does not independently measure codec/PWM frequency",
        "snapshot_limit": "PCM, PWM and TDM counters are read at different instants; records may be partial in raw logs",
    }
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
