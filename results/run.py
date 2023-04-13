import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from multiprocessing import Pool
from typing import List

def get_score_from_file(file: Path) -> float:
    for line in reversed(file.read_text(encoding="utf-8").splitlines()):
        if line.startswith("Solution score = "):
            return float(line.split(" = ")[1])

    return 0.0

def update_overview() -> None:
    scores_by_solver = {}
    outputs_root = Path(__file__).parent.parent / "results" / "output"

    for directory in outputs_root.iterdir():
        scores_by_input = {}

        for file in directory.iterdir():
            if file.name.endswith(".out"):
                scores_by_input[int(file.stem)] = get_score_from_file(file)

        scores_by_solver[directory.name] = scores_by_input

    overview_template_file = Path(__file__).parent.parent / "results" / "overview.tmpl.html"
    overview_file = Path(__file__).parent.parent / "results" / "overview.html"

    overview_template = overview_template_file.read_text(encoding="utf-8")
    overview = overview_template.replace("/* scores_by_solver */{}", json.dumps(scores_by_solver))

    with overview_file.open("w+", encoding="utf-8") as file:
        file.write(overview)

    print(f"Overview: file://{overview_file.resolve()}")

def run_input(solver: Path, input: int, output_directory: Path) -> int:
    runner = Path(__file__).parent / "local_runner.py"
    input_file = Path(__file__).parent / "input" / f"{input:02}"
    baseline_file = Path(__file__).parent / "input" / f"{input:02}.a"

    output_file = output_directory / f"{input:02}.out"
    logs_file = output_directory / f"{input:02}.log"

    environment = dict(os.environ)
    environment["LOCAL"] = "1"

    with output_file.open("wb+") as output:
        with logs_file.open("wb+") as logs:
            try:
                process = subprocess.run([sys.executable, str(runner), str(input_file), str(baseline_file), "--", str(solver)],
                                          env=environment,
                                          stdout=output,
                                          stderr=logs,
                                          timeout=500)

                if process.returncode != 0:
                    raise RuntimeError(f"Judge exited with status code {process.returncode} for input {input}")

                return get_score_from_file(output_file)
            except subprocess.TimeoutExpired:
                raise RuntimeError(f"Judge timed out for input {input}")

def run(solver: Path, inputs: List[int], output_directory: Path) -> None:
    if not output_directory.is_dir():
        output_directory.mkdir(parents=True)

    with Pool() as pool:
        scores = pool.starmap(run_input, [(solver, input, output_directory) for input in inputs])

    for i, input in enumerate(inputs):
        print(f"{input}: {scores[i]:,.3f}")

    if len(inputs) > 1:
        print(f"Total score: {sum(scores):,.3f}")

def main() -> None:
    parser = argparse.ArgumentParser(description="Run a solver.")
    parser.add_argument("solver", type=str, help="the solver to run")
    parser.add_argument("--input", type=int, help="the input to run (defaults to 1-11)")

    args = parser.parse_args()

    solver = Path(__file__).parent.parent / "cmake-build-release" / args.solver
    if not solver.is_file():
        raise RuntimeError(f"Solver not found, {solver} is not a file")

    output_directory = Path(__file__).parent / "output" / args.solver

    if args.input is None:
        run(solver, list(range(1, 12)), output_directory)
    else:
        run(solver, [args.input], output_directory)

    update_overview()

if __name__ == "__main__":
    main()
