#!/usr/bin/env python3
"""
sf2_piano_audit.py — read-only SF2 velocity-layer audit.

No SoundFont is modified. Parses RIFF/SF2 chunks directly and reports:
- presets and instruments
- instrument zones (key/velocity ranges, sample, filter settings)
- velocity layers, sample storage estimates, and optional audio features
- effective zones after intersecting preset and instrument key/velocity ranges
- per-layer sample estimates and same-MIDI-note comparisons across velocity layers

Requirements:
    Python 3.10+
    Optional for audio feature analysis: pip install numpy

Examples:
    python sf2_piano_audit.py piano.sf2 --list
    python sf2_piano_audit.py piano.sf2 --instrument "Grand Piano"
    python sf2_piano_audit.py piano.sf2 --instrument 0 --out piano_report
    python sf2_piano_audit.py piano.sf2 --preset "Grand Piano" --out piano_report
    python sf2_piano_audit.py piano.sf2 --instrument "Grand Piano" --audio-features

Outputs: *_zones.csv (effective zones), *_layers.csv (velocity bands),
*_note_comparison.csv (same MIDI notes compared across layers).
"""

from __future__ import annotations
import argparse
import csv
import math
import struct
import sys
from collections import defaultdict
from pathlib import Path

# SF2 generator operator numbers
GEN_FILTER_FC = 8
GEN_FILTER_Q = 9
GEN_KEY_RANGE = 43
GEN_VEL_RANGE = 44
GEN_ATTENUATION = 48
GEN_SAMPLE_ID = 53
GEN_INSTRUMENT = 41

# SF2 fixed record sizes
RECORD_SIZES = {
    b"phdr": 38, b"pbag": 4, b"pmod": 10, b"pgen": 4,
    b"inst": 22, b"ibag": 4, b"imod": 10, b"igen": 4,
    b"shdr": 46,
}


def parse_riff_chunks(data: bytes, start: int, end: int):
    """Yield (chunk_id, payload_start, payload_size), respecting RIFF padding."""
    pos = start
    while pos + 8 <= end:
        cid = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        payload = pos + 8
        if size < 0 or payload + size > len(data):
            raise ValueError(f"Malformed chunk {cid!r} at offset {pos}")
        yield cid, payload, size
        pos = payload + size + (size & 1)


def find_sf2_chunks(path: Path):
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"sfbk":
        raise ValueError("File is not a RIFF/sfbk SoundFont 2 file.")
    chunks = {}
    for cid, p, n in parse_riff_chunks(data, 12, min(len(data), 8 + struct.unpack_from("<I", data, 4)[0])):
        if cid == b"LIST" and n >= 4:
            list_type = data[p:p + 4]
            for scid, sp, sn in parse_riff_chunks(data, p + 4, p + n):
                if list_type == b"pdta":
                    chunks[scid] = (sp, sn)
                elif list_type == b"sdta" and scid == b"smpl":
                    chunks[b"smpl"] = (sp, sn)
    return data, chunks


def cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("latin-1", errors="replace").strip()


def unpack_records(data: bytes, chunk, record_size: int, fmt: str):
    if chunk is None:
        raise ValueError("Required SF2 chunk is missing.")
    p, n = chunk
    count = n // record_size
    size = struct.calcsize(fmt)
    if size != record_size:
        raise RuntimeError(f"Internal record size mismatch for {fmt}: {size} != {record_size}")
    return [struct.unpack_from(fmt, data, p + i * record_size) for i in range(count)]


def signed16(v):
    return v - 65536 if v >= 32768 else v


def gen_amount(data, op):
    # SF2 generator amount is a 16-bit union. keyRange/velRange are unsigned bytes.
    if op in (GEN_KEY_RANGE, GEN_VEL_RANGE):
        lo, hi = data & 0xff, (data >> 8) & 0xff
        return (lo, hi)
    return signed16(data)


def generators_for_bag(data, chunks, bag_chunk_name, gen_chunk_name, bag_index):
    bag_p, bag_n = chunks[bag_chunk_name]
    gen_p, gen_n = chunks[gen_chunk_name]
    bag_count = bag_n // 4
    if bag_index < 0 or bag_index >= bag_count:
        return {}
    gen_start = struct.unpack_from("<H", data, bag_p + bag_index * 4)[0]
    gen_end = struct.unpack_from("<H", data, bag_p + (bag_index + 1) * 4)[0]
    result = {}
    for gi in range(gen_start, min(gen_end, gen_n // 4)):
        op, amount = struct.unpack_from("<HH", data, gen_p + gi * 4)
        result[op] = gen_amount(amount, op)
    return result


def merge_generators(global_gens, local_gens):
    """Merge a global zone with a local zone, intersecting range generators."""
    merged = dict(global_gens or {})
    local_gens = local_gens or {}
    for op, value in local_gens.items():
        if op in (GEN_KEY_RANGE, GEN_VEL_RANGE) and op in merged:
            glo, ghi = merged[op]
            llo, lhi = value
            merged[op] = (max(glo, llo), min(ghi, lhi))
        else:
            merged[op] = value
    return merged


def intersect_range(a, b):
    """Return intersection of inclusive SF2 ranges, or None if disjoint."""
    lo, hi = max(a[0], b[0]), min(a[1], b[1])
    return (lo, hi) if lo <= hi else None


def midi_note_name(note):
    names = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
    return f"{names[note % 12]}{note // 12 - 1}"


def range_value(gens, op, default):
    v = gens.get(op, default)
    return v if isinstance(v, tuple) else default


def parse_soundfont(path: Path):
    data, chunks = find_sf2_chunks(path)
    required = [b"phdr", b"pbag", b"pgen", b"inst", b"ibag", b"igen", b"shdr", b"smpl"]
    missing = [x.decode() for x in required if x not in chunks]
    if missing:
        raise ValueError("Missing required SF2 chunks: " + ", ".join(missing))

    presets_raw = unpack_records(data, chunks[b"phdr"], 38, "<20sHHHIII")
    pbags = unpack_records(data, chunks[b"pbag"], 4, "<HH")
    instruments_raw = unpack_records(data, chunks[b"inst"], 22, "<20sH")
    ibags = unpack_records(data, chunks[b"ibag"], 4, "<HH")
    shdr_raw = unpack_records(data, chunks[b"shdr"], 46, "<20sIIIIIBbHH")
    smpl_p, smpl_n = chunks[b"smpl"]

    presets = []
    for i, r in enumerate(presets_raw[:-1]):  # final record is terminal
        name, program, bank, bag_idx, *_ = r
        end_bag = presets_raw[i + 1][3]
        zones = []
        global_gens = {}
        for bi in range(bag_idx, end_bag):
            gens = generators_for_bag(data, chunks, b"pbag", b"pgen", bi)
            if GEN_INSTRUMENT in gens:
                zones.append({"zone_index": bi - bag_idx,
                              "instrument_id": gens[GEN_INSTRUMENT],
                              "gens": merge_generators(global_gens, gens)})
            else:
                global_gens = gens
        presets.append({
            "name": cstr(name), "program": program, "bank": bank,
            "zones": zones,
        })

    instruments = []
    for i, r in enumerate(instruments_raw[:-1]):  # final record is terminal
        name, bag_idx = r
        end_bag = instruments_raw[i + 1][1]
        zones = []
        global_gens = {}
        for bi in range(bag_idx, end_bag):
            gens = generators_for_bag(data, chunks, b"ibag", b"igen", bi)
            if GEN_SAMPLE_ID in gens:
                zones.append({"zone_index": bi - bag_idx,
                              "sample_id": gens[GEN_SAMPLE_ID],
                              "gens": merge_generators(global_gens, gens)})
            else:
                global_gens = gens
        instruments.append({"name": cstr(name), "zones": zones})

    samples = []
    for r in shdr_raw[:-1]:  # final record is terminal
        name, start, end, loop_start, loop_end, rate, root, correction, link, stype = r
        samples.append({
            "name": cstr(name), "start": start, "end": end,
            "loop_start": loop_start, "loop_end": loop_end,
            "rate": rate, "root": root, "correction": correction,
            "link": link, "type": stype,
            "frames": max(0, end - start),
            "bytes": max(0, end - start) * 2,
        })

    return data, chunks, presets, instruments, samples, smpl_p, smpl_n


def fmt_range(r):
    return f"{r[0]}-{r[1]}"


def features_for_sample(data, smpl_p, sample, max_points=16000):
    """Optional approximate features, using evenly-spaced samples from PCM."""
    try:
        import numpy as np
    except ImportError:
        return None
    frames = sample["frames"]
    if frames <= 0:
        return {"rms": 0.0, "centroid_hz": 0.0, "hf_ratio": 0.0}
    count = min(frames, max_points)
    if count == frames:
        indices = np.arange(frames, dtype=np.int64)
    else:
        indices = np.linspace(0, frames - 1, count, dtype=np.int64)
    byte_offsets = smpl_p + (sample["start"] + indices) * 2
    # Efficient enough for moderate sample counts; read 16-bit signed PCM at selected indices.
    vals = np.fromiter(
        (struct.unpack_from("<h", data, int(off))[0] for off in byte_offsets),
        dtype=np.float32, count=count
    ) / 32768.0
    rms = float(np.sqrt(np.mean(vals * vals)))
    # A decimated sample is not a faithful audio spectrum; use FFT only as a rough timbral descriptor.
    centered = vals - float(np.mean(vals))
    if len(centered) < 64 or not np.any(centered):
        return {"rms": rms, "centroid_hz": 0.0, "hf_ratio": 0.0}
    window = np.hanning(len(centered))
    spec = np.abs(np.fft.rfft(centered * window))
    freqs = np.fft.rfftfreq(len(centered), d=1.0 / max(1, sample["rate"]))
    total = float(np.sum(spec)) + 1e-12
    centroid = float(np.sum(freqs * spec) / total)
    hf = float(np.sum(spec[freqs >= 4000]) / total)
    return {"rms": rms, "centroid_hz": centroid, "hf_ratio": hf}


def main():
    ap = argparse.ArgumentParser(description="Read-only SF2 piano velocity-layer audit")
    ap.add_argument("sf2", type=Path, help="input .sf2 file")
    ap.add_argument("--list", action="store_true", help="list presets and instruments, then exit")
    ap.add_argument("--instrument", help="instrument name substring or zero-based instrument index")
    ap.add_argument("--preset", help="optional preset name substring or zero-based preset index")
    ap.add_argument("--out", default="sf2_audit", help="output prefix (default: sf2_audit)")
    ap.add_argument("--audio-features", action="store_true",
                    help="estimate sample RMS/spectral descriptors (requires numpy; approximate)")
    args = ap.parse_args()

    try:
        data, chunks, presets, instruments, samples, smpl_p, smpl_n = parse_soundfont(args.sf2)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2

    print(f"File: {args.sf2}")
    print(f"File size: {args.sf2.stat().st_size:,} bytes")
    print(f"Sample data (smpl): {smpl_n:,} bytes (~{smpl_n / 2:,.0f} 16-bit points)")
    print(f"Presets: {len(presets)}; instruments: {len(instruments)}; samples: {len(samples)}")

    if args.list or (args.instrument is None and args.preset is None):
        print("\nPRESETS (index, bank, program, name)")
        for i, p in enumerate(presets):
            print(f"  {i:3d}: bank={p['bank']:3d} program={p['program']:3d}  {p['name']}")
        print("\nINSTRUMENTS (index, name, zones)")
        for i, inst in enumerate(instruments):
            print(f"  {i:3d}: {inst['name']} ({len(inst['zones'])} sample zones)")
        if args.instrument is None:
            print("\nUse --instrument INDEX_OR_NAME to audit a specific instrument.")
            return 0

    # Resolve instrument selector.
    selected_instruments = None
    if args.instrument is not None:
        try:
            ii = int(args.instrument)
            if ii < 0 or ii >= len(instruments):
                raise ValueError("instrument index out of range")
            selected_instruments = [ii]
        except ValueError as e:
            if str(e) == "instrument index out of range":
                print(f"ERROR: {e}", file=sys.stderr)
                return 2
            needle = args.instrument.casefold()
            selected_instruments = [i for i, x in enumerate(instruments) if needle in x["name"].casefold()]
            if not selected_instruments:
                print(f"ERROR: no instrument matches {args.instrument!r}", file=sys.stderr)
                return 2

    # Resolve preset selector, if supplied.
    chosen_presets = None
    if args.preset is not None:
        try:
            pi = int(args.preset)
            if pi < 0 or pi >= len(presets):
                raise ValueError("preset index out of range")
            chosen_presets = [pi]
        except ValueError as e:
            if str(e) == "preset index out of range":
                print(f"ERROR: {e}", file=sys.stderr)
                return 2
            needle = args.preset.casefold()
            chosen_presets = [i for i, x in enumerate(presets) if needle in x["name"].casefold()]
            if not chosen_presets:
                print(f"ERROR: no preset matches {args.preset!r}", file=sys.stderr)
                return 2

    # If the user selected an instrument but not a preset, find all presets that
    # reference it. This is essential: preset zones can impose velocity ranges
    # that are invisible when instrument zones are inspected in isolation.
    if chosen_presets is None and selected_instruments is not None:
        selected_set = set(selected_instruments)
        chosen_presets = [pi for pi, p in enumerate(presets)
                          if any(z["instrument_id"] in selected_set for z in p["zones"])]

    # If no selector was given, preserve the list-only behavior above.
    if chosen_presets is None:
        chosen_presets = []

    if chosen_presets:
        print("\nSelected presets:")
        for pi in chosen_presets:
            p = presets[pi]
            print(f"  {pi}: {p['name']} (bank {p['bank']}, program {p['program']})")

    feature_cache = {}
    rows = []
    effective_mode = bool(chosen_presets)

    if effective_mode:
        # Expand preset zones -> instrument sample zones. The effective key and
        # velocity ranges are intersections across both hierarchy levels.
        for pi in chosen_presets:
            preset = presets[pi]
            for pz in preset["zones"]:
                ii = pz["instrument_id"]
                if ii < 0 or ii >= len(instruments):
                    continue
                if selected_instruments is not None and ii not in selected_instruments:
                    continue
                inst = instruments[ii]
                pg = pz["gens"]
                pkr = range_value(pg, GEN_KEY_RANGE, (0, 127))
                pvr = range_value(pg, GEN_VEL_RANGE, (0, 127))
                for iz in inst["zones"]:
                    sid = iz["sample_id"]
                    if sid < 0 or sid >= len(samples):
                        continue
                    ig = iz["gens"]
                    ikr = range_value(ig, GEN_KEY_RANGE, (0, 127))
                    ivr = range_value(ig, GEN_VEL_RANGE, (0, 127))
                    kr = intersect_range(pkr, ikr)
                    vr = intersect_range(pvr, ivr)
                    if kr is None or vr is None:
                        continue
                    s = samples[sid]
                    # Filter generators and attenuation are additive between
                    # preset and instrument levels when both are explicitly set.
                    cutoff_values = [g[GEN_FILTER_FC] for g in (pg, ig) if isinstance(g.get(GEN_FILTER_FC), int)]
                    q_values = [g[GEN_FILTER_Q] for g in (pg, ig) if isinstance(g.get(GEN_FILTER_Q), int)]
                    att_values = [g[GEN_ATTENUATION] for g in (pg, ig) if isinstance(g.get(GEN_ATTENUATION), int)]
                    cutoff = sum(cutoff_values) if cutoff_values else None
                    q = sum(q_values) if q_values else None
                    attenuation = sum(att_values) if att_values else 0
                    cutoff_hz = (8.176 * (2 ** (cutoff / 1200.0))) if cutoff is not None else None
                    row = {
                        "preset_index": pi, "preset": preset["name"],
                        "bank": preset["bank"], "program": preset["program"],
                        "preset_zone_index": pz["zone_index"],
                        "instrument_index": ii, "instrument": inst["name"],
                        "instrument_zone_index": iz["zone_index"],
                        "sample_id": sid, "sample": s["name"],
                        "key_lo": kr[0], "key_hi": kr[1],
                        "velocity_lo": vr[0], "velocity_hi": vr[1],
                        "sample_frames": s["frames"], "sample_bytes": s["bytes"],
                        "sample_rate": s["rate"], "root_key": s["root"],
                        "loop_frames": max(0, s["loop_end"] - s["loop_start"]),
                        "filter_cutoff_raw_cents": cutoff if cutoff is not None else "",
                        "filter_cutoff_hz_approx": round(cutoff_hz, 2) if cutoff_hz is not None else "",
                        "filter_q_raw_cB": q if q is not None else "",
                        "attenuation_raw_cB": attenuation,
                        "rms": "", "centroid_hz_approx": "", "hf_ratio_4khz_approx": "",
                    }
                    if args.audio_features:
                        if sid not in feature_cache:
                            feature_cache[sid] = features_for_sample(data, smpl_p, s)
                        f = feature_cache[sid]
                        if f:
                            row["rms"] = round(f["rms"], 6)
                            row["centroid_hz_approx"] = round(f["centroid_hz"], 1)
                            row["hf_ratio_4khz_approx"] = round(f["hf_ratio"], 4)
                    rows.append(row)

        print("\nEFFECTIVE PRESET + INSTRUMENT ZONES")
        print("  Velocity/key ranges below are intersections of preset-zone and instrument-zone ranges.")
        for r in rows:
            print(f"  preset={r['preset']!r}, instrument={r['instrument']!r}, "
                  f"vel {r['velocity_lo']:3d}-{r['velocity_hi']:3d}, "
                  f"key {r['key_lo']:3d}-{r['key_hi']:3d}, sample={r['sample']!r}")
    else:
        # No preset maps the selected instrument (or the user did not select an
        # instrument). Fall back to instrument-only data and make the limitation clear.
        if selected_instruments is None:
            print("\nNo instrument or preset selected; use --list or pass --preset/--instrument.")
            return 0
        print("\nWARNING: no matching preset zones found; reporting instrument zones only. "
              "Preset-level velocity ranges cannot be included in this mode.")
        for ii in selected_instruments:
            inst = instruments[ii]
            print(f"\nINSTRUMENT {ii}: {inst['name']} ({len(inst['zones'])} sample zones)")
            for iz in inst["zones"]:
                sid = iz["sample_id"]
                if sid < 0 or sid >= len(samples):
                    continue
                s = samples[sid]
                g = iz["gens"]
                kr = range_value(g, GEN_KEY_RANGE, (0, 127))
                vr = range_value(g, GEN_VEL_RANGE, (0, 127))
                cutoff = g.get(GEN_FILTER_FC)
                q = g.get(GEN_FILTER_Q)
                cutoff_hz = (8.176 * (2 ** (cutoff / 1200.0))) if isinstance(cutoff, int) else None
                row = {
                    "preset_index": "", "preset": "", "bank": "", "program": "",
                    "preset_zone_index": "", "instrument_index": ii, "instrument": inst["name"],
                    "instrument_zone_index": iz["zone_index"], "sample_id": sid, "sample": s["name"],
                    "key_lo": kr[0], "key_hi": kr[1], "velocity_lo": vr[0], "velocity_hi": vr[1],
                    "sample_frames": s["frames"], "sample_bytes": s["bytes"], "sample_rate": s["rate"],
                    "root_key": s["root"], "loop_frames": max(0, s["loop_end"] - s["loop_start"]),
                    "filter_cutoff_raw_cents": cutoff if isinstance(cutoff, int) else "",
                    "filter_cutoff_hz_approx": round(cutoff_hz, 2) if cutoff_hz is not None else "",
                    "filter_q_raw_cB": q if isinstance(q, int) else "",
                    "attenuation_raw_cB": g.get(GEN_ATTENUATION, 0),
                    "rms": "", "centroid_hz_approx": "", "hf_ratio_4khz_approx": "",
                }
                if args.audio_features:
                    if sid not in feature_cache:
                        feature_cache[sid] = features_for_sample(data, smpl_p, s)
                    f = feature_cache[sid]
                    if f:
                        row["rms"] = round(f["rms"], 6)
                        row["centroid_hz_approx"] = round(f["centroid_hz"], 1)
                        row["hf_ratio_4khz_approx"] = round(f["hf_ratio"], 4)
                rows.append(row)

    prefix = Path(args.out)
    zones_csv = prefix.with_name(prefix.name + "_zones.csv")
    fields = list(rows[0].keys()) if rows else ["preset", "instrument", "velocity_lo", "velocity_hi"]
    with zones_csv.open("w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nWrote effective zone report: {zones_csv}")

    # Distinct effective velocity bands are the candidate layers. A layer is
    # only considered present for a note if its key range covers that MIDI key.
    layer_groups = defaultdict(list)
    for r in rows:
        layer_groups[(r["preset"], r["instrument"], r["velocity_lo"], r["velocity_hi"])].append(r)
    layer_rows = []
    for (preset_name, inst_name, vlo, vhi), group in sorted(layer_groups.items(), key=lambda x: (x[0][0], x[0][1], x[0][2], x[0][3])):
        unique_samples = {r["sample_id"]: r for r in group}
        layer_rows.append({
            "preset": preset_name, "instrument": inst_name,
            "velocity_lo": vlo, "velocity_hi": vhi,
            "zones": len(group), "unique_samples": len(unique_samples),
            "unique_sample_bytes_est": sum(r["sample_bytes"] for r in unique_samples.values()),
            "key_min": min(r["key_lo"] for r in group), "key_max": max(r["key_hi"] for r in group),
            "notes_covered": len({k for r in group for k in range(r["key_lo"], r["key_hi"] + 1)}),
            "mean_cutoff_hz": round(sum(float(r["filter_cutoff_hz_approx"] or 0) for r in group) /
                                    max(1, sum(bool(r["filter_cutoff_hz_approx"]) for r in group)), 1)
                              if any(r["filter_cutoff_hz_approx"] != "" for r in group) else "",
        })
    layers_csv = prefix.with_name(prefix.name + "_layers.csv")
    with layers_csv.open("w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=list(layer_rows[0].keys()) if layer_rows else
                                ["preset", "instrument", "velocity_lo", "velocity_hi"])
        writer.writeheader()
        writer.writerows(layer_rows)
    print(f"Wrote velocity-layer summary: {layers_csv}")

    # Comparison table: one row per preset/instrument/MIDI note, with samples
    # and zones grouped by effective velocity band. This directly compares the
    # same key across all velocity layers instead of comparing unrelated notes.
    by_preset_inst_note = defaultdict(lambda: defaultdict(list))
    for r in rows:
        base = (r["preset"], r["instrument"])
        for note in range(r["key_lo"], r["key_hi"] + 1):
            band = (r["velocity_lo"], r["velocity_hi"])
            by_preset_inst_note[(*base, note)][band].append(r)
    compare_rows = []
    for (preset_name, inst_name, note), bands in sorted(by_preset_inst_note.items()):
        ordered_bands = sorted(bands)
        if len(ordered_bands) < 2:
            continue
        out = {"preset": preset_name, "instrument": inst_name,
               "midi_note": note, "note_name": midi_note_name(note),
               "layer_count_for_note": len(ordered_bands)}
        for layer_no, (vlo, vhi) in enumerate(ordered_bands, 1):
            group = bands[(vlo, vhi)]
            out[f"layer_{layer_no}_velocity"] = f"{vlo}-{vhi}"
            out[f"layer_{layer_no}_samples"] = " | ".join(sorted({r["sample"] for r in group}))
            out[f"layer_{layer_no}_sample_ids"] = " | ".join(str(x) for x in sorted({r["sample_id"] for r in group}))
            cutoffs = sorted({str(r["filter_cutoff_hz_approx"]) for r in group if r["filter_cutoff_hz_approx"] != ""})
            qs = sorted({str(r["filter_q_raw_cB"]) for r in group if r["filter_q_raw_cB"] != ""})
            out[f"layer_{layer_no}_cutoff_hz"] = " | ".join(cutoffs)
            out[f"layer_{layer_no}_filter_q"] = " | ".join(qs)
        compare_rows.append(out)
    compare_csv = prefix.with_name(prefix.name + "_note_comparison.csv")
    compare_fields = list(compare_rows[0].keys()) if compare_rows else ["preset", "instrument", "midi_note", "note_name"]
    with compare_csv.open("w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=compare_fields)
        writer.writeheader()
        writer.writerows(compare_rows)
    print(f"Wrote same-note layer comparison: {compare_csv}")

    print("\nEFFECTIVE VELOCITY LAYERS")
    layer_counts = defaultdict(set)
    for lr in layer_rows:
        layer_counts[(lr["preset"], lr["instrument"])].add((lr["velocity_lo"], lr["velocity_hi"]))
        print(f"  preset={lr['preset']!r}, instrument={lr['instrument']!r}: "
              f"vel {lr['velocity_lo']:3d}-{lr['velocity_hi']:3d}, {lr['zones']} zones, "
              f"{lr['unique_samples']} unique samples, ~{lr['unique_sample_bytes_est']:,} bytes referenced PCM, "
              f"{lr['notes_covered']} notes covered")
    for (preset_name, inst_name), bands in sorted(layer_counts.items()):
        print(f"  TOTAL: preset={preset_name!r}, instrument={inst_name!r}: {len(bands)} distinct velocity bands")
        if len(bands) != 11:
            print("    Note: this mapping yields a layer-band count other than 11; inspect the CSV for "
                  "multiple overlapping bands or preset mappings. The script reports ranges found in the SF2,")
            print("    it does not assume that every band corresponds to a perceptually distinct piano sample layer.")

    if args.audio_features:
        print("\nAudio descriptors are rough sample-level statistics, not perceptual similarity scores.")
    print("Compare rows in *_note_comparison.csv to inspect the same MIDI note across velocity bands.")
    print("Sample-byte estimates count referenced samples per band; stereo pairs/shared sample data can affect removable-size estimates.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
