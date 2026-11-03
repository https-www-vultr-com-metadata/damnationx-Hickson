#!/usr/bin/env python3
"""Run fresh native Survivors campaigns at real 20 Hz, in parallel isolated profiles.

Requires a TCP-enabled DAMNATION_S1_BRIDGE build and the user's Diablo data.
The production bot issues ordinary movement, gift and belt-potion commands. This
runner never edits hero stats, grants XP, skips acts, kills enemies or speeds time.
A failed/dead run is reported as a failure rather than called a full-length pass.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def free_ports(count: int) -> list[int]:
    sockets = []
    try:
        for _ in range(count):
            listener = socket.socket()
            listener.bind(("127.0.0.1", 0))
            sockets.append(listener)
        return [listener.getsockname()[1] for listener in sockets]
    finally:
        for listener in sockets:
            listener.close()


def snapshot_runtime(binary: Path, output: Path) -> Path:
    """Keep later builds from changing the executable used by an ongoing run."""
    bundle = next((parent for parent in binary.parents if parent.suffix == ".app"), None)
    destination = output / "runtime"
    if bundle:
        copied = destination / bundle.name
        shutil.copytree(bundle, copied, symlinks=True)
        return copied / binary.relative_to(bundle)
    destination.mkdir()
    copied = destination / binary.name
    shutil.copy2(binary, copied)
    # Non-bundle builds may use a colocated native core / image library.
    for pattern in ("*.dylib", "*.so", "*.dll"):
        for library in binary.parent.glob(pattern):
            shutil.copy2(library, destination / library.name)
    for name in ("devilutionx.mpq", "spawn.mpq"):
        resource = binary.parent / name
        if resource.exists():
            shutil.copy2(resource, destination / name)
    return copied


def read_profile(path: Path):
    """Read-only verification of the production profile's version/FNV checksum."""
    if not path.exists():
        return None
    try:
        content = path.read_bytes()
        if len(content) > 4096:
            return None
        payload, checksum_text = content.rsplit(b"checksum ", 1)
        checksum = 2166136261
        for value in payload:
            checksum = ((checksum ^ value) * 16777619) & 0xFFFFFFFF
        if int(checksum_text.strip()) != checksum:
            return None
        words = payload.decode("utf-8").split()
        if words[:2] != ["DamnationXSurvivors", "1"] or len(words) != 16:
            return None
        values = [int(value) for value in words[2:]]
        names = ["shards", "kills", "runs", "acts", "bosses", "victories", "best_seconds",
                 "best_level", "unlocked_difficulty", "selected_difficulty"]
        return {**dict(zip(names, values[:10])), "permanent_ranks": values[10:]}
    except (OSError, ValueError, UnicodeError):
        return None


def leave_own_process(process: subprocess.Popen, bridge_port: int) -> None:
    if process.poll() is not None:
        return
    try:
        with socket.create_connection(("127.0.0.1", bridge_port), timeout=1) as bridge:
            bridge.settimeout(2)
            bridge.sendall(b"leave\n")
            bridge.recv(4096)
        process.wait(timeout=8)
    except (OSError, subprocess.TimeoutExpired):
        # This is exactly the Popen child created by this runner, never a PID
        # discovered through a process-name search or a user's running game.
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


class NativeRun:
    def __init__(self, hero_class: str, folder: Path, binary: Path, data: Path,
                 game_port: int, bridge_port: int, extra_args: list[str], join: str | None = None):
        self.hero_class = hero_class
        self.folder = folder
        self.folder.mkdir()
        self.save = folder / "save"
        self.config = folder / "config"
        self.save.mkdir()
        self.config.mkdir()
        self.telemetry = folder / "run.jsonl"
        self.native_log = (folder / "native.log").open("w")
        self.bridge_port = bridge_port
        self.game_port = game_port
        self.started = time.monotonic()
        self.stopped_at = None
        self.offset = 0
        self.partial = ""
        self.events = 0
        self.last = {}
        self.final = None
        self.max_act = 0
        self.min_life_ratio = 1.0
        self.damage_events = 0
        self.gift_items: set[int] = set()
        self.minimum_damage_hp = None
        self.reason = "running"
        (self.config / "diablo.ini").write_text(
            f"[Network]\nPort={game_port}\nBind Address=127.0.0.1\n"
            "[Game]\nSpeed=20\nFriendly Fire=0\nPause Game When Window Loses Focus=0\n"
            "[Graphics]\nVSync=0\n", encoding="utf-8")
        command = [str(binary), "--headless", "--bot-survivors",
                   "--bot-transport", "tcp", "--bot-hero", f"QA{hero_class.title()}",
                   "--bot-class", hero_class, "--data-dir", str(data),
                   "--save-dir", str(self.save), "--config-dir", str(self.config), *extra_args]
        command += ["--bot-host"] if join is None else ["--bot-join", join]
        environment = os.environ.copy()
        environment.update({"DAMNATION_BRIDGE_PORT": str(bridge_port),
                            "DAMNATIONX_SURVIVORS_PLAYTEST": "1",
                            "DAMNATIONX_SURVIVORS_PLAYTEST_LOG": str(self.telemetry)})
        self.process = subprocess.Popen(command, stdout=self.native_log, stderr=subprocess.STDOUT,
                                        cwd=binary.parent, env=environment)
        (folder / "launch.json").write_text(json.dumps({"command": command, "pid": self.process.pid,
              "game_port": game_port, "bridge_port": bridge_port, "fresh_profile": True,
              "clock": "native real-time 20Hz", "cheats": False}, indent=2))

    def poll(self):
        if self.telemetry.exists():
            with self.telemetry.open("r", encoding="utf-8") as source:
                source.seek(self.offset)
                self.partial += source.read()
                self.offset = source.tell()
            lines = self.partial.split("\n")
            self.partial = lines.pop()
            for line in lines:
                if not line:
                    continue
                event = json.loads(line)
                self.events += 1
                kind = event.get("kind")
                if kind == "gift":
                    self.gift_items.add(event["item"])
                elif kind == "damage":
                    self.damage_events += 1
                    hp = event.get("after_fp", 0) / 64
                    self.minimum_damage_hp = hp if self.minimum_damage_hp is None else min(hp, self.minimum_damage_hp)
                elif kind in ("tick", "finished"):
                    self.last = event
                    self.max_act = max(self.max_act, event.get("act", 0))
                    if event.get("max_hp", 0):
                        self.min_life_ratio = min(self.min_life_ratio, event["hp"] / event["max_hp"])
                    if kind == "finished":
                        self.final = event
                        self.reason = "victory" if event.get("victory") else "death"
                        break
        if self.final:
            self.stopped_at = time.monotonic()
            leave_own_process(self.process, self.bridge_port)
            return True
        if self.process.poll() is not None:
            self.reason = "native_process_exited"
            self.stopped_at = time.monotonic()
            return True
        return False

    def report(self):
        final = self.final or self.last
        seconds = final.get("sim_seconds", 0)
        wall_ms = final.get("wall_ms", 0)
        elapsed = (self.stopped_at or time.monotonic()) - self.started
        real_time = bool(seconds and wall_ms >= seconds * 1000 * 0.97 and elapsed >= seconds * 0.97 and final.get("tick_rate") == 20)
        success = bool(self.final and self.final.get("victory") and seconds >= 1200
                       and self.max_act == 5 and real_time)
        campaign_files = sorted(str(path.relative_to(self.save)) for pattern in ("*.sv", "*.hsv", "stash*")
                                for path in self.save.glob(pattern))
        profile = read_profile(self.save / "survivors-profile.dat")
        settled_once = bool(profile and self.final and profile["runs"] == 1
                            and profile["victories"] == int(bool(self.final.get("victory")))
                            and profile["kills"] == self.final.get("kills")
                            and profile["best_seconds"] == seconds
                            and profile["best_level"] == self.final.get("level"))
        progression_ok = bool(profile and profile["runs"] == 1 and profile["victories"] == 1
                              and profile["acts"] == 5 and profile["bosses"] == 5
                              and profile["best_seconds"] >= 1200 and profile["unlocked_difficulty"] == 1
                              and profile["permanent_ranks"] == [0, 0, 0, 0])
        return {"class": self.hero_class, "outcome": self.reason,
                "successful_full_length_native_run": success and not campaign_files and progression_ok,
                "verified_profile": profile, "fresh_progression_settled_once": settled_once,
                "fresh_campaign_completion_verified": progression_ok,
                "native_real_time_not_accelerated": real_time,
                "sim_seconds": seconds, "native_wall_ms": wall_ms,
                "runner_wall_seconds": round(elapsed, 3),
                "native_tick_rate": final.get("tick_rate"),
                "max_act": self.max_act, "final": final,
                "damage_events": self.damage_events, "lowest_recorded_life_ratio": self.min_life_ratio,
                "minimum_hp_from_damage_events": self.minimum_damage_hp,
                "gift_items_exercised": sorted(self.gift_items), "telemetry_events": self.events,
                "profile_saved": (self.save / "survivors-profile.dat").exists(),
                "campaign_files_created": campaign_files, "native_exit_code": self.process.poll(),
                "telemetry": str(self.telemetry), "native_log": str(self.folder / "native.log")}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--classes", nargs="+", choices=("warrior", "rogue", "sorcerer"),
                        default=["warrior", "rogue", "sorcerer"])
    parser.add_argument("--co-op", action="store_true", help="Join all classes into one real host-authoritative party instead of independent campaigns")
    parser.add_argument("--timeout", type=float, default=3600, help="Wall seconds per campaign, no simulation acceleration")
    parser.add_argument("--startup-timeout", type=float, default=120)
    parser.add_argument("--progress-seconds", type=float, default=30)
    parser.add_argument("--game-arg", action="append", default=[])
    args = parser.parse_args()
    binary = args.binary.resolve()
    data = args.data_dir.resolve()
    if not binary.is_file() or not data.is_dir():
        parser.error("binary must be a file and --data-dir must be an existing Diablo data folder")
    if len(set(args.classes)) != len(args.classes):
        parser.error("--classes must not contain duplicates")
    output = args.output_dir.resolve() if args.output_dir else Path(tempfile.mkdtemp(prefix="damnationx-survivors-native-"))
    output.mkdir(parents=True, exist_ok=True)
    if (output / "runtime").exists() or any((output / name).exists() for name in args.classes):
        parser.error("output already contains a runtime or class profiles; choose a fresh output folder")
    original_hash = sha256(binary)
    copied = snapshot_runtime(binary, output)
    assert sha256(copied) == original_hash
    runs = []
    active = []
    next_progress = time.monotonic()
    result = {"kind": "native_full_length_campaign_playtest", "binary_sha256": original_hash,
              "binary": str(copied), "clock": "native real-time 20Hz", "cheats": False,
              "output": str(output), "mode": "cooperative_party" if args.co_op else "independent_campaigns", "runs": []}
    try:
        ports = free_ports(len(args.classes) * 2)
        for index, hero_class in enumerate(args.classes):
            join = None
            game_port = ports[index * 2]
            if args.co_op and index > 0:
                deadline = time.monotonic() + args.startup_timeout
                while not runs[0].telemetry.exists():
                    if runs[0].process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("Co-op authority failed to initialize its real game")
                    time.sleep(0.1)
                game_port = ports[0]
                join = f"127.0.0.1:{game_port}"
            run = NativeRun(hero_class, output / hero_class, copied, data,
                            game_port, ports[index * 2 + 1], args.game_arg, join)
            runs.append(run)
            active.append(run)
            print(json.dumps({"started": hero_class, "pid": run.process.pid,
                              "game_port": run.game_port, "bridge_port": run.bridge_port}), flush=True)
        while active:
            now = time.monotonic()
            for run in active[:]:
                complete = run.poll()
                if not complete and run.events == 0 and now - run.started > args.startup_timeout:
                    run.reason = "native_startup_timeout"
                    leave_own_process(run.process, run.bridge_port)
                    complete = True
                if not complete and now - run.started > args.timeout:
                    run.reason = "native_run_timeout"
                    leave_own_process(run.process, run.bridge_port)
                    complete = True
                if complete:
                    active.remove(run)
                    print(json.dumps({"completed": run.hero_class, **run.report()}), flush=True)
            if now >= next_progress:
                next_progress = now + args.progress_seconds
                print(json.dumps({"progress": [{"class": run.hero_class, "act": run.last.get("act"),
                       "sim_seconds": run.last.get("sim_seconds"), "hp": run.last.get("hp"),
                       "kills": run.last.get("kills")} for run in active]}), flush=True)
            result["runs"] = [run.report() for run in runs]
            (output / "progress.json").write_text(json.dumps(result, indent=2))
            time.sleep(0.5)
    except KeyboardInterrupt:
        for run in active:
            run.reason = "runner_interrupted"
        return_code = 130
    else:
        return_code = 0 if runs and all(run.report()["successful_full_length_native_run"] for run in runs) else 1
    finally:
        for run in runs:
            leave_own_process(run.process, run.bridge_port)
            run.native_log.close()
        result["runs"] = [run.report() for run in runs]
        result["all_classes_cleared"] = bool(runs and all(run.report()["successful_full_length_native_run"] for run in runs))
        (output / "result.json").write_text(json.dumps(result, indent=2))
        print(json.dumps({"result": str(output / "result.json"), "all_classes_cleared": result["all_classes_cleared"]}), flush=True)
    return return_code


if __name__ == "__main__":
    sys.exit(main())
