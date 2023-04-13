import hcr
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).parent.parent

solvers = sorted([file.stem for file in (PROJECT_ROOT / "cmake-build-release").iterdir() if file.is_file() and file.name.startswith("v")])
default_seeds = list(range(1, 12))
is_maximizing = True
results_directory = PROJECT_ROOT / "results"

def run_solver(solver: str, input: int) -> hcr.Output:
    return hcr.run_process([sys.executable,
                            PROJECT_ROOT / "scripts" / "local_runner.py",
                            PROJECT_ROOT / "scripts" / "input" / f"{input:02}",
                            PROJECT_ROOT / "scripts" / "input" / f"{input:02}.a",
                            "--",
                            PROJECT_ROOT / "cmake-build-release" / solver],
                            timeout=15)

def get_score(output: hcr.Output) -> float:
    for line in reversed(output.stdout.splitlines()):
        if line.startswith("Solution score = "):
            return float(line.split(" = ")[1])

    return 0.0

if __name__ == "__main__":
    hcr.cli(solvers, default_seeds, is_maximizing, results_directory, get_score, run_solver=run_solver)
