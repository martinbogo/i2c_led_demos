#!/usr/bin/env python3
"""Automated audio-lab for the Elevated soundtrack.

This script can:
- optionally build the local `elevated` binary
- export one or more audio render variants to WAV
- align each candidate against a reference WAV
- score each variant on spectral balance, width, level, and dynamics
- run a coarse post-processing search to test whether the mismatch is
  better explained by mastering/capture differences than by synth logic

Requires: numpy
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import subprocess
import sys
import wave
from dataclasses import asdict, dataclass
from itertools import product
from typing import Iterable

import numpy as np

DEFAULT_VARIANTS = [
    "current",
    "hi-precision",
    "hi-precision-denorm",
    "hi-precision-denorm-x87",
]
DEFAULT_WINDOWS = [(18.0, 10.0), (60.0, 10.0), (120.0, 10.0), (170.0, 10.0)]
EPSILON = 1e-12


@dataclass
class WindowMetrics:
    centroid: float
    low_band: float
    upper_mid_band: float
    rms: float
    crest: float
    side_mid_ratio: float
    correlation: float


@dataclass
class VariantResult:
    name: str
    path: str
    sha256: str
    lag_seconds: float
    lag_correlation: float
    score: float
    windows: dict[str, WindowMetrics]


@dataclass
class PostFxResult:
    variant: str
    score: float
    width: float
    low_gain: float
    high_gain: float
    drive: float
    cutoff_hz: float


class AnalysisError(RuntimeError):
    pass


def parse_windows(spec: str) -> list[tuple[float, float]]:
    windows: list[tuple[float, float]] = []
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        try:
            start_str, dur_str = item.split(":", 1)
            windows.append((float(start_str), float(dur_str)))
        except ValueError as exc:
            raise argparse.ArgumentTypeError(
                f"invalid window '{item}', expected START:DURATION"
            ) from exc
    if not windows:
        raise argparse.ArgumentTypeError("at least one analysis window is required")
    return windows


def run_command(command: list[str], cwd: pathlib.Path | None = None) -> None:
    completed = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    if completed.returncode != 0:
        raise AnalysisError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )


def load_wav(path: pathlib.Path) -> tuple[int, np.ndarray]:
    with wave.open(str(path), "rb") as handle:
        sample_rate = handle.getframerate()
        channels = handle.getnchannels()
        sample_width = handle.getsampwidth()
        frames = handle.getnframes()
        raw = handle.readframes(frames)

    if channels != 2:
        raise AnalysisError(f"{path} is not stereo")
    if sample_width != 2:
        raise AnalysisError(f"{path} is not 16-bit PCM")

    data = np.frombuffer(raw, dtype="<i2").astype(np.float64).reshape(-1, channels)
    data /= 32768.0
    return sample_rate, data


def write_wav(path: pathlib.Path, sample_rate: int, data: np.ndarray) -> None:
    clipped = np.clip(data, -1.0, 1.0)
    pcm = np.rint(clipped * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(2)
        handle.setsampwidth(2)
        handle.setframerate(sample_rate)
        handle.writeframes(pcm.tobytes())


def mono_mix(stereo: np.ndarray) -> np.ndarray:
    return stereo.mean(axis=1)


def rms_envelope(signal: np.ndarray, window: int = 4096, hop: int = 1024) -> np.ndarray:
    if signal.size < window:
        return np.array([float(np.sqrt(np.mean(signal * signal)))], dtype=np.float64)
    count = 1 + (signal.size - window) // hop
    envelope = np.empty(count, dtype=np.float64)
    for index in range(count):
        start = index * hop
        frame = signal[start:start + window]
        envelope[index] = np.sqrt(np.mean(frame * frame))
    return envelope


def best_lag_seconds(
    reference_mono: np.ndarray,
    candidate_mono: np.ndarray,
    sample_rate: int,
    start_seconds: float,
    end_seconds: float,
    max_lag_seconds: float,
) -> tuple[float, float]:
    window = 4096
    hop = 1024
    start = max(0, int(start_seconds * sample_rate))
    end = min(reference_mono.size, candidate_mono.size, int(end_seconds * sample_rate))
    if end - start < window:
        return 0.0, 0.0

    ref_env = rms_envelope(reference_mono[start:end], window=window, hop=hop)
    cand_env = rms_envelope(candidate_mono[start:end], window=window, hop=hop)
    max_lag_steps = int(max_lag_seconds * sample_rate / hop)
    best_lag = 0
    best_corr = -1.0

    for lag in range(-max_lag_steps, max_lag_steps + 1):
        if lag < 0:
            ref_slice = ref_env[-lag:]
            cand_slice = cand_env[:ref_slice.size]
        elif lag > 0:
            ref_slice = ref_env[:-lag]
            cand_slice = cand_env[lag:lag + ref_slice.size]
        else:
            ref_slice = ref_env
            cand_slice = cand_env

        if ref_slice.size < 8 or cand_slice.size != ref_slice.size:
            continue

        ref_centered = ref_slice - ref_slice.mean()
        cand_centered = cand_slice - cand_slice.mean()
        denom = np.sqrt(np.sum(ref_centered * ref_centered) * np.sum(cand_centered * cand_centered))
        corr = 0.0 if denom <= EPSILON else float(np.sum(ref_centered * cand_centered) / denom)
        if corr > best_corr:
            best_corr = corr
            best_lag = lag

    return best_lag * hop / sample_rate, best_corr


def extract_window(stereo: np.ndarray, sample_rate: int, start_seconds: float, duration_seconds: float) -> np.ndarray:
    start = int(round(start_seconds * sample_rate))
    end = int(round((start_seconds + duration_seconds) * sample_rate))
    start = max(0, min(start, stereo.shape[0]))
    end = max(start, min(end, stereo.shape[0]))
    return stereo[start:end]


def accumulated_spectrum(mono: np.ndarray, sample_rate: int) -> tuple[np.ndarray, np.ndarray]:
    if mono.size < 4096:
        padded = np.pad(mono, (0, max(0, 4096 - mono.size)))
        windowed = padded * np.hanning(padded.size)
        magnitudes = np.abs(np.fft.rfft(windowed))
        freqs = np.fft.rfftfreq(windowed.size, 1.0 / sample_rate)
        return freqs, magnitudes

    fft_window = 4096
    hop = 1024
    window = np.hanning(fft_window)
    total = None
    count = 0
    for start in range(0, mono.size - fft_window, hop):
        frame = mono[start:start + fft_window] * window
        magnitudes = np.abs(np.fft.rfft(frame))
        total = magnitudes if total is None else total + magnitudes
        count += 1
    if total is None:
        total = np.zeros(fft_window // 2 + 1, dtype=np.float64)
    total /= max(count, 1)
    freqs = np.fft.rfftfreq(fft_window, 1.0 / sample_rate)
    return freqs, total


def window_metrics(stereo: np.ndarray, sample_rate: int) -> WindowMetrics:
    mono = mono_mix(stereo)
    rms = float(np.sqrt(np.mean(mono * mono)))
    peak = float(np.max(np.abs(mono))) if mono.size else 0.0
    crest = peak / max(rms, EPSILON)

    mid = 0.5 * (stereo[:, 0] + stereo[:, 1])
    side = 0.5 * (stereo[:, 0] - stereo[:, 1])
    mid_rms = float(np.sqrt(np.mean(mid * mid)))
    side_rms = float(np.sqrt(np.mean(side * side)))
    side_mid_ratio = side_rms / max(mid_rms, EPSILON)

    left = stereo[:, 0]
    right = stereo[:, 1]
    if left.size < 2 or np.std(left) <= EPSILON or np.std(right) <= EPSILON:
        correlation = 1.0
    else:
        correlation = float(np.corrcoef(left, right)[0, 1])

    freqs, magnitudes = accumulated_spectrum(mono, sample_rate)
    power = magnitudes * magnitudes + EPSILON
    centroid = float(np.sum(freqs * power) / np.sum(power))
    low_band = float(np.sum(power[(freqs >= 0.0) & (freqs < 200.0)]) / np.sum(power))
    upper_mid_band = float(np.sum(power[(freqs >= 1000.0) & (freqs < 4000.0)]) / np.sum(power))

    return WindowMetrics(
        centroid=centroid,
        low_band=low_band,
        upper_mid_band=upper_mid_band,
        rms=rms,
        crest=crest,
        side_mid_ratio=side_mid_ratio,
        correlation=correlation,
    )


def metric_distance(reference: WindowMetrics, candidate: WindowMetrics) -> float:
    rms_log = abs(math.log((candidate.rms + EPSILON) / (reference.rms + EPSILON)))
    crest_norm = abs(candidate.crest - reference.crest) / max(reference.crest, 1.0)
    centroid_norm = abs(candidate.centroid - reference.centroid) / max(reference.centroid, 1.0)
    return (
        2.0 * centroid_norm
        + 12.0 * abs(candidate.low_band - reference.low_band)
        + 12.0 * abs(candidate.upper_mid_band - reference.upper_mid_band)
        + 5.0 * abs(candidate.side_mid_ratio - reference.side_mid_ratio)
        + 3.0 * abs(candidate.correlation - reference.correlation)
        + 2.0 * rms_log
        + 1.5 * crest_norm
    )


def score_variant(
    reference: np.ndarray,
    candidate: np.ndarray,
    sample_rate: int,
    windows: list[tuple[float, float]],
    lag_seconds: float,
) -> tuple[float, dict[str, WindowMetrics]]:
    metrics: dict[str, WindowMetrics] = {}
    total_score = 0.0

    for start_seconds, duration_seconds in windows:
        ref_window = extract_window(reference, sample_rate, start_seconds, duration_seconds)
        cand_window = extract_window(candidate, sample_rate, start_seconds + lag_seconds, duration_seconds)
        ref_metrics = window_metrics(ref_window, sample_rate)
        cand_metrics = window_metrics(cand_window, sample_rate)
        label = f"{start_seconds:g}:{duration_seconds:g}"
        metrics[label] = cand_metrics
        total_score += metric_distance(ref_metrics, cand_metrics)

    return total_score / max(len(windows), 1), metrics


def sha256_short(path: pathlib.Path) -> str:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return digest[:16]


def render_variant(binary: pathlib.Path, variant: str, output_path: pathlib.Path) -> None:
    command = [str(binary)]
    if variant != "current":
        command += ["--audio-variant", variant]
    command += ["--write-audio", str(output_path)]
    run_command(command, cwd=binary.parent)


def analyse_variants(
    binary: pathlib.Path,
    reference_path: pathlib.Path,
    output_dir: pathlib.Path,
    variants: list[str],
    windows: list[tuple[float, float]],
    max_lag_seconds: float,
) -> tuple[list[VariantResult], int, np.ndarray]:
    sample_rate, reference = load_wav(reference_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    results: list[VariantResult] = []
    ref_mono = mono_mix(reference)

    for variant in variants:
        output_path = output_dir / f"{variant}.wav"
        render_variant(binary, variant, output_path)
        candidate_rate, candidate = load_wav(output_path)
        if candidate_rate != sample_rate:
            raise AnalysisError(
                f"sample-rate mismatch: {variant} is {candidate_rate} Hz, reference is {sample_rate} Hz"
            )

        trim_frames = min(reference.shape[0], candidate.shape[0])
        reference_trimmed = reference[:trim_frames]
        candidate_trimmed = candidate[:trim_frames]
        lag_seconds, lag_corr = best_lag_seconds(
            mono_mix(reference_trimmed),
            mono_mix(candidate_trimmed),
            sample_rate,
            start_seconds=10.0,
            end_seconds=40.0,
            max_lag_seconds=max_lag_seconds,
        )
        score, metrics = score_variant(
            reference_trimmed,
            candidate_trimmed,
            sample_rate,
            windows,
            lag_seconds,
        )
        results.append(
            VariantResult(
                name=variant,
                path=str(output_path),
                sha256=sha256_short(output_path),
                lag_seconds=lag_seconds,
                lag_correlation=lag_corr,
                score=score,
                windows=metrics,
            )
        )

    results.sort(key=lambda item: item.score)
    return results, sample_rate, reference


def one_pole_lowpass(signal: np.ndarray, sample_rate: int, cutoff_hz: float) -> np.ndarray:
    if signal.size == 0:
        return signal.copy()
    alpha = math.exp(-2.0 * math.pi * cutoff_hz / sample_rate)
    output = np.empty_like(signal)
    output[0] = signal[0]
    for index in range(1, signal.size):
        output[index] = (1.0 - alpha) * signal[index] + alpha * output[index - 1]
    return output


def apply_postfx(stereo: np.ndarray, sample_rate: int, width: float, low_gain: float, high_gain: float, drive: float, cutoff_hz: float) -> np.ndarray:
    low = np.empty_like(stereo)
    for channel in range(2):
        low[:, channel] = one_pole_lowpass(stereo[:, channel], sample_rate, cutoff_hz)
    high = stereo - low
    processed = low * low_gain + high * high_gain

    mid = 0.5 * (processed[:, 0] + processed[:, 1])
    side = 0.5 * (processed[:, 0] - processed[:, 1]) * width
    processed = np.column_stack((mid + side, mid - side))

    if drive > 1.0 + EPSILON:
        norm = math.tanh(drive)
        processed = np.tanh(processed * drive) / max(norm, EPSILON)

    return np.clip(processed, -1.0, 1.0)


def search_postfx(
    variant_name: str,
    variant_path: pathlib.Path,
    reference: np.ndarray,
    sample_rate: int,
    windows: list[tuple[float, float]],
    lag_seconds: float,
    cutoff_hz: float,
) -> PostFxResult:
    _, candidate = load_wav(variant_path)
    trim_frames = min(reference.shape[0], candidate.shape[0])
    reference = reference[:trim_frames]
    candidate = candidate[:trim_frames]

    best: PostFxResult | None = None
    for width, low_gain, high_gain, drive in product(
        [1.0, 1.15, 1.3, 1.45, 1.6],
        [0.5, 0.65, 0.8, 1.0],
        [1.0, 1.2, 1.4, 1.6, 1.8],
        [1.0, 1.1, 1.25, 1.4],
    ):
        total_score = 0.0
        for start_seconds, duration_seconds in windows:
            ref_window = extract_window(reference, sample_rate, start_seconds, duration_seconds)
            cand_window = extract_window(candidate, sample_rate, start_seconds + lag_seconds, duration_seconds)
            processed = apply_postfx(cand_window, sample_rate, width, low_gain, high_gain, drive, cutoff_hz)
            total_score += metric_distance(
                window_metrics(ref_window, sample_rate),
                window_metrics(processed, sample_rate),
            )

        score = total_score / max(len(windows), 1)
        if best is None or score < best.score:
            best = PostFxResult(
                variant=variant_name,
                score=score,
                width=width,
                low_gain=low_gain,
                high_gain=high_gain,
                drive=drive,
                cutoff_hz=cutoff_hz,
            )

    assert best is not None
    return best


def maybe_build(repo_root: pathlib.Path) -> pathlib.Path:
    binary = repo_root / "elevated"
    run_command(["make", "elevated"], cwd=repo_root)
    return binary


def summarise_results(results: Iterable[VariantResult]) -> None:
    print("Variant ranking (lower score is better):")
    for result in results:
        print(
            f"  {result.name:24s} score={result.score:.6f} "
            f"lag={result.lag_seconds:+.3f}s corr={result.lag_correlation:.4f} sha={result.sha256}"
        )
        intro = result.windows.get("18:10")
        if intro is not None:
            print(
                "    intro "
                f"centroid={intro.centroid:.2f} low={intro.low_band:.5f} "
                f"upper_mid={intro.upper_mid_band:.5f} rms={intro.rms:.5f} "
                f"width={intro.side_mid_ratio:.5f} corr={intro.correlation:.5f}"
            )


def serialise_results(results: list[VariantResult], postfx: PostFxResult | None) -> dict:
    payload = {
        "variants": [
            {
                "name": result.name,
                "path": result.path,
                "sha256": result.sha256,
                "lag_seconds": result.lag_seconds,
                "lag_correlation": result.lag_correlation,
                "score": result.score,
                "windows": {name: asdict(metrics) for name, metrics in result.windows.items()},
            }
            for result in results
        ]
    }
    if postfx is not None:
        payload["postfx"] = asdict(postfx)
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=".", help="repository root containing the elevated binary")
    parser.add_argument("--binary", default=None, help="path to the elevated binary; defaults to REPO/elevated")
    parser.add_argument("--reference", default="elevated.wav", help="reference WAV recording")
    parser.add_argument("--output-dir", default="/tmp/elevated-audio-lab", help="where exported variant WAVs should be written")
    parser.add_argument("--variants", nargs="+", default=DEFAULT_VARIANTS, help="audio variants to render and compare")
    parser.add_argument(
        "--windows",
        type=parse_windows,
        default=DEFAULT_WINDOWS,
        help="comma-separated START:DURATION windows, for example 18:10,60:10",
    )
    parser.add_argument("--max-lag-seconds", type=float, default=5.0, help="maximum lag search in seconds")
    parser.add_argument("--cutoff-hz", type=float, default=700.0, help="post-fx low/high crossover for the coarse search")
    parser.add_argument("--skip-postfx", action="store_true", help="skip the coarse post-fx search")
    parser.add_argument("--json-output", default=None, help="optional path for a JSON result dump")
    parser.add_argument("--build", action="store_true", help="build the elevated binary before analysis")
    parser.add_argument("--write-best-postfx", default=None, help="optional path to write the best post-fx processed WAV")
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo).resolve()
    reference_path = pathlib.Path(args.reference)
    if not reference_path.is_absolute():
        reference_path = repo_root / reference_path
    output_dir = pathlib.Path(args.output_dir).resolve()

    if args.build:
        binary = maybe_build(repo_root)
    else:
        binary = pathlib.Path(args.binary).resolve() if args.binary else (repo_root / "elevated").resolve()
    if not binary.exists():
        raise AnalysisError(f"binary not found: {binary}")
    if not reference_path.exists():
        raise AnalysisError(f"reference WAV not found: {reference_path}")

    results, sample_rate, reference = analyse_variants(
        binary=binary,
        reference_path=reference_path,
        output_dir=output_dir,
        variants=args.variants,
        windows=args.windows,
        max_lag_seconds=args.max_lag_seconds,
    )
    summarise_results(results)

    postfx_result: PostFxResult | None = None
    if not args.skip_postfx and results:
        best_variant = results[0]
        postfx_result = search_postfx(
            variant_name=best_variant.name,
            variant_path=pathlib.Path(best_variant.path),
            reference=reference,
            sample_rate=sample_rate,
            windows=args.windows,
            lag_seconds=best_variant.lag_seconds,
            cutoff_hz=args.cutoff_hz,
        )
        print(
            "Best coarse post-fx search on the leading variant: "
            f"variant={postfx_result.variant} score={postfx_result.score:.6f} "
            f"width={postfx_result.width:.2f} low_gain={postfx_result.low_gain:.2f} "
            f"high_gain={postfx_result.high_gain:.2f} drive={postfx_result.drive:.2f} "
            f"cutoff_hz={postfx_result.cutoff_hz:.1f}"
        )

        if args.write_best_postfx:
            _, stereo = load_wav(pathlib.Path(best_variant.path))
            processed = apply_postfx(
                stereo,
                sample_rate,
                postfx_result.width,
                postfx_result.low_gain,
                postfx_result.high_gain,
                postfx_result.drive,
                postfx_result.cutoff_hz,
            )
            write_wav(pathlib.Path(args.write_best_postfx), sample_rate, processed)
            print(f"Wrote best post-fx WAV to {args.write_best_postfx}")

    if args.json_output:
        payload = serialise_results(results, postfx_result)
        pathlib.Path(args.json_output).write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"Wrote JSON summary to {args.json_output}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AnalysisError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
