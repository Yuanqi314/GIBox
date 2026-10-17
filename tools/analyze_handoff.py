#!/usr/bin/env python3
"""Summarize schema-1 GIBox handoff logs; stdlib only, no device calls."""
import argparse
import collections
import json
from pathlib import Path


def summarize(events, malformed=0):
    # PID reuse can append another process capture to the same filename.
    captures = [e.get("capture_start_ns") for e in events if e.get("kind") == "meta"]
    latest = captures[-1] if captures else None
    ignored_captures = bool(latest is not None and any(e.get("capture_start_ns") != latest for e in events))
    if latest is not None:
        events = [e for e in events if e.get("capture_start_ns") == latest]
    events = sorted(events, key=lambda e: e.get("seq", 0))
    snaps = [e for e in events if e.get("kind") == "snapshot"]
    setters = [e for e in events if e.get("kind") == "mesh_setter"]
    completed = {e["call"] for e in setters if e.get("phase") == "return"}
    warnings = []
    if ignored_captures:
        warnings.append("Multiple captures in file: summarizing only the last meta.capture_start_ns.")
    if malformed:
        warnings.append(f"{malformed} malformed/truncated JSON lines")
    if any(e.get("contaminated") for e in events):
        warnings.append("Replacement was enabled or GIBox assigned an object: use a fresh process with replacement off for baseline inference.")
    if any(e.get("dropped", 0) or e.get("write_errors", 0) for e in events):
        warnings.append("Log loss or file-write error reported; missing events cannot establish absence.")
    if any(e.get("tick_conflict") for e in events):
        warnings.append("Tick observed on multiple threads; Unity diagnostic reads are disabled after the conflict.")
    for kind in ("budget_exhausted", "setter_budget_exhausted", "install_failed", "probe_error"):
        if any(e.get("kind") == kind for e in events):
            warnings.append(kind)
    if not snaps:
        warnings.append("No snapshots. Check skip.reason, handoff-type log and startup fingerprint errors.")

    def root_id(e):
        return (e.get("root"), e.get("root_native"), e.get("root_native_id"))

    def renderer_id(r):
        return (r.get("renderer"), r.get("renderer_native"), r.get("renderer_native_id"))

    history = collections.defaultdict(list)
    handoffs = []
    for s in snaps:
        key = root_id(s)
        if s.get("phase") == "activate_enter":
            prev = history[key]
            # Only the latest observed preparation cycle, not all uses of an
            # entity address (entity pointers and pooled objects are reused).
            prior_activation = max((x["seq"] for x in prev if x.get("phase") in ("activate_enter", "activate_leave")), default=0)
            starts = [i for i, x in enumerate(prev) if x["seq"] > prior_activation and x.get("phase") == "ready_enter"]
            start = starts[-1] if starts else len(prev)
            cycle = prev[start:] if starts else []
            # Include the nearest earlier resource handoff for this root, if
            # present; this is an observation, not proof of an allocation.
            early = [x for x in prev[:start] if x["seq"] > prior_activation
                     and x.get("phase") in ("instantiate_return", "resource_receive_enter")]
            if early:
                cycle = [early[-1]] + cycle
            reference = cycle[0] if cycle else None
            before = {renderer_id(r): r for r in (reference or {}).get("renderers", [])}
            item = {
                "activation_seq": s["seq"], "entity": s.get("entity"),
                "config_id": s.get("config_id"), "root": s.get("root"),
                "activation_frame": s.get("frame"),
                "root_currently_inactive": s.get("active_hierarchy") is False,
                "reference_phase": (reference or {}).get("phase"),
                "reference_frame": (reference or {}).get("frame"),
                "same_frame_as_reference": bool(reference and reference.get("frame") == s.get("frame")),
                "first_draw_proven": False, "renderers": [],
            }
            for r in s.get("renderers", []):
                b = before.get(renderer_id(r))
                calls = [e for e in setters if e.get("phase") == "enter"
                         and e.get("renderer") == r.get("renderer")
                         and reference and reference["seq"] < e["seq"] < s["seq"]]
                mats_complete = bool(b is not None and not b.get("materials_truncated") and not r.get("materials_truncated"))
                item["renderers"].append({
                    "renderer": r.get("renderer"), "name": r.get("name"),
                    "mesh_at_reference": b.get("mesh") if b else None,
                    "mesh_at_activation": r.get("mesh"),
                    "mesh_changed": (b.get("mesh"), b.get("mesh_native"), b.get("mesh_native_id")) != (r.get("mesh"), r.get("mesh_native"), r.get("mesh_native_id")) if b else None,
                    "material_elements_changed": b.get("materials") != r.get("materials") if mats_complete else None,
                    "external_setter_calls_in_window": [e["call"] for e in calls if e.get("source") == "external"],
                    "completed_external_setter_calls": [e["call"] for e in calls if e.get("source") == "external" and e["call"] in completed],
                })
            handoffs.append(item)
        history[key].append(s)

    return {
        "schema": 1, "first_draw_proven": False,
        "snapshot_count": len(snaps),
        "phases": dict(collections.Counter(s.get("phase") for s in snaps)),
        "snapshot_tids": sorted({s["tid"] for s in snaps}),
        "tick_tids": sorted({e["tick_tid"] for e in events if e.get("tick_tid")}),
        "all_snapshot_tids_match_tick": all(s.get("tid") == s.get("tick_tid") for s in snaps) if snaps else None,
        "mesh_present_at_handoff": [
            {"seq": s["seq"], "phase": s["phase"], "root": s.get("root"),
             "renderer_count": s.get("renderer_count", 0),
             "meshes_present": sum(r.get("mesh") not in (None, "0x0") for r in s.get("renderers", [])),
             "root_active_hierarchy": s.get("active_hierarchy")}
            for s in snaps if s.get("phase") in ("instantiate_return", "resource_receive_enter")
        ],
        "handoffs": handoffs, "warnings": warnings,
        "interpretation_limit": "A mesh observed at handoff was already present at that boundary. No observed setter does not prove none ran. Inactive before this activation does not prove never drawn. Sync/pool/async branches need separate coverage.",
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("log", type=Path)
    p.add_argument("--out", type=Path)
    args = p.parse_args()
    events, malformed = [], 0
    for line in args.log.read_text(encoding="utf-8").splitlines():
        try:
            e = json.loads(line)
            if not isinstance(e, dict):
                raise ValueError("object required")
            events.append(e)
        except (ValueError, TypeError):
            malformed += 1
    text = json.dumps(summarize(events, malformed), ensure_ascii=False, indent=2) + "\n"
    if args.out:
        args.out.write_text(text, encoding="utf-8")
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
