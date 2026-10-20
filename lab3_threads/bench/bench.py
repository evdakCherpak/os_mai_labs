import os
import random
import re
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "morph")
OUT = os.path.join(ROOT, "bench", "results")
REPEATS = int(os.environ.get("REPEATS", "9"))

SPLITS = ("static", "dynamic")
THREADS = (1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 24, 32)
WINDOW_THREADS = (1, 2, 3, 4, 5, 6, 8, 12, 16)

EXPERIMENTS = {
    "size": [(s, n, 3, 10, THREADS) for s in SPLITS for n in (50, 100, 200, 500, 1000, 2000)],
    "window": [(s, 1000, w, 4, WINDOW_THREADS) for s in SPLITS for w in (1, 3, 5, 9, 15)],
}


def run_once(split, rows, window, iterations, threads):
    args = [BINARY, "-t", str(threads), "-g", f"{rows}x{rows}", "-w", str(window), "-k", str(iterations), "-q", "-v"]
    if split == "static":
        args.insert(1, "-S")
    result = subprocess.run(args, capture_output=True, text=True, check=True)
    return float(re.search(r"time_ms=([0-9.]+)", result.stderr).group(1))


def main():
    subprocess.run(["make", "-s", "-C", ROOT, "morph"], check=True)
    os.makedirs(OUT, exist_ok=True)

    points = [
        (name, (split, rows, window, iterations, threads))
        for name, series in EXPERIMENTS.items()
        for split, rows, window, iterations, thread_counts in series
        for threads in thread_counts
    ]
    times = {point: [] for point in points}
    shuffler = random.Random(12)
    for repeat in range(REPEATS):
        print(f"pass {repeat + 1}/{REPEATS}", file=sys.stderr)
        order = points[:]
        shuffler.shuffle(order)
        for point in order:
            times[point].append(run_once(*point[1]))

    for name, series in EXPERIMENTS.items():
        with open(os.path.join(OUT, f"{name}.csv"), "w") as out:
            out.write("split,rows,window,iterations,threads,time_ms,speedup,efficiency\n")
            for split, rows, window, iterations, thread_counts in series:
                base = statistics.median(times[(name, (split, rows, window, iterations, 1))])
                for threads in thread_counts:
                    elapsed = statistics.median(times[(name, (split, rows, window, iterations, threads))])
                    speedup = base / elapsed
                    out.write(
                        f"{split},{rows},{window},{iterations},{threads},"
                        f"{elapsed:.3f},{speedup:.3f},{speedup / threads:.3f}\n"
                    )


if __name__ == "__main__":
    main()
