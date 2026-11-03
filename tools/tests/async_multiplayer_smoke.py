#!/usr/bin/env python3
"""Two native processes over TCP; optional proxy latency, isolated saves/config.

Build with DAMNATION_S1_BRIDGE=ON and TCP enabled. The S1 bridge binds localhost
only. This driver never reads or modifies a user's campaign/save folder.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class Bridge:
    def __init__(self, port, timeout=45, process=None, state_file=None):
        self.state_file = state_file
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.socket = socket.create_connection(("127.0.0.1", port), timeout=1)
                self.socket.settimeout(5)
                self.reader = self.socket.makefile("rb")
                return
            except OSError:
                if process is not None and process.poll() is not None:
                    raise RuntimeError(f"Native process exited before bridge startup: {process.returncode}")
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"S1 bridge did not start on {port}")
                time.sleep(0.1)

    def command(self, command):
        self.socket.sendall(command.encode() + b"\n")
        line = self.reader.readline()
        if not line:
            raise RuntimeError("Game closed its QA bridge")
        result = json.loads(line)
        if command == "state" and self.state_file is not None:
            self.state_file.write_text(json.dumps(result, indent=2))
        return result

    def wait(self, predicate, timeout=30, observe=None):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.command("state")
            if observe is not None:
                observe()
            if predicate(last):
                return last
            time.sleep(0.1)
        raise AssertionError(f"Timed out waiting for native state: {last}")


class DelayProxy:
    def __init__(self, target, delay):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(1)
        self.target = target
        self.delay = delay
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        incoming, _ = self.listener.accept()
        upstream = socket.create_connection(("127.0.0.1", self.target))
        for source, destination in ((incoming, upstream), (upstream, incoming)):
            threading.Thread(target=self.forward, args=(source, destination), daemon=True).start()

    def forward(self, source, destination):
        # Preserve TCP's byte order while adding latency without limiting its
        # throughput. Packet loss/reordering are injected by the protocol tests.
        import queue
        pending = queue.Queue()

        def writer():
            try:
                while True:
                    due, data = pending.get()
                    if data is None:
                        break
                    time.sleep(max(0, due - time.monotonic()))
                    destination.sendall(data)
            except OSError:
                pass
            finally:
                try:
                    destination.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

        threading.Thread(target=writer, daemon=True).start()
        try:
            while data := source.recv(65536):
                pending.put((time.monotonic() + self.delay, data))
        except OSError:
            pass
        finally:
            pending.put((0, None))


def walk_near(bridge, npc):
    from collections import deque
    state = bridge.command("state")
    rows = bridge.command("map")["rows"]
    start = state["x"], state["y"]
    queue, previous = deque([start]), {start: None}
    goal = None
    while queue:
        tile = queue.popleft()
        if max(abs(tile[0] - npc["x"]), abs(tile[1] - npc["y"])) <= npc.get("radius", 1):
            goal = tile
            break
        for dx, dy in ((1,0),(-1,0),(0,1),(0,-1),(1,1),(-1,-1),(1,-1),(-1,1)):
            nxt = tile[0] + dx, tile[1] + dy
            if 0 <= nxt[1] < len(rows) and 0 <= nxt[0] < len(rows[0]) and rows[nxt[1]][nxt[0]] == "1" and nxt not in previous:
                previous[nxt] = tile
                queue.append(nxt)
    if goal is None:
        raise AssertionError(f"No native walkable route to {npc}")
    path = []
    while goal != start:
        path.append(goal)
        goal = previous[goal]
    path.reverse()
    for tile in path[7::8] + path[-1:]:
        bridge.command(f"walk {tile[0]} {tile[1]}")
        bridge.wait(lambda state: (state.get("x"), state.get("y")) == tile
                    and state["async"]["pendingInputs"] == 0, timeout=20)


def check_commerce(peer):
    initial = peer.command("state")
    healer = next(npc for npc in initial["towners"] if "pepin" in npc["name"].lower())
    walk_near(peer, healer)
    assert peer.command("show healer buy")["ok"]
    offer = next(item for item in peer.command("shop")["items"]
                 if item["merchant"] == "healer" and item["healPotion"] and item["price"] <= initial["gold"])
    before = peer.command("state")
    queued = peer.command(f"buy healer {offer['id']}")
    assert queued.get("queued"), queued
    bought = peer.wait(lambda state: state["gold"] == before["gold"] - offer["price"]
                       and state["potions"] == before["potions"] + 1
                       and state["async"]["pendingInputs"] == 0)
    peer.command("close store")
    assert peer.command("potion")["ok"]
    consumed = peer.wait(lambda state: state["potions"] == bought["potions"] - 1
                         and state["async"]["pendingInputs"] == 0)
    return {"offer": offer, "bought": bought, "consumed": consumed}


def check_class_utility(host, peer):
    initial = peer.command("state")
    skills = [spell for spell in initial["spells"] if spell["source"] == "skill"]
    skill = next(spell for spell in skills if "repair" in spell["name"].lower() or "recharge" in spell["name"].lower())
    recharge = "recharge" in skill["name"].lower()
    field, maximum = ("charges", "maxCharges") if recharge else ("dur", "maxdur")
    equipped = next(item for item in initial["equipment"] if item["name"] and 1 < item[maximum] < 255)
    slot = equipped["slot"]
    amount = max(2, equipped[maximum] // 4)
    fixture = "drain-charge" if recharge else "wear"
    prepared = host.command(f"qa {fixture} {initial['playerId']} {slot} {amount}")
    assert prepared.get("fixture"), prepared
    damaged = peer.wait(lambda state: any(item["slot"] == slot and item[field] < item[maximum]
                                         for item in state["equipment"]))
    token = damaged["utility"]["token"]
    cast = peer.command(f"cast skill {skill['id']} self")
    assert cast["ok"], cast
    issued = peer.wait(lambda state: state["utility"]["ready"] and state["utility"]["token"] != token)
    assert issued["utility"]["spell"] == skill["id"], issued["utility"]
    assert peer.command(f"target item {slot}")["queued"]
    applied = peer.wait(lambda state: not state["utility"]["ready"] and state["async"]["pendingInputs"] == 0
                       and any(item["slot"] == slot and item[field] == item[maximum]
                               and item[maximum] < equipped[maximum] for item in state["equipment"]))
    assert applied["mana"] == damaged["mana"], (damaged["mana"], applied["mana"])
    assert not peer.command(f"target item {slot}")["ok"], "A consumed utility lease must not replay"
    return {"skill": skill, "fixture": prepared, "issued": issued, "applied": applied}


def scroll_count(state, spell):
    return sum(item.get("spell") == spell and item.get("scroll", True)
               for item in state["inventory"] + state["belt"])


def cast_scroll_utility(peer, name, source="scroll"):
    before = peer.command("state")
    spell = next(value for value in before["spells"]
                 if value["source"] == source and value["name"].casefold() == name.casefold())
    count = scroll_count(before, spell["id"])
    cast = peer.command(f"cast {source} {spell['id']} self")
    assert cast["ok"], cast
    issued = peer.wait(lambda state: state["utility"]["ready"] and state["utility"]["spell"] == spell["id"]
                       and state["utility"]["token"] != before["utility"]["token"])
    if source == "scroll":
        assert scroll_count(issued, spell["id"]) == count - 1, (name, count, issued)
        assert issued["mana"] == before["mana"], (before["mana"], issued["mana"])
    else:
        assert issued["mana"] == before["mana"] - spell["manaCost"], (name, before["mana"], spell["manaCost"], issued["mana"])
    return {"spell": spell, "issued": issued, "source": source, "scrolls_before": count, "mana_before": before["mana"]}


def check_deep_utilities(host, peer):
    initial = peer.command("state")
    setup = host.command(f"qa utility-items {initial['playerId']}")
    assert setup.get("fixture"), setup
    peer.wait(lambda state: any(item["magical"] and not item["identified"] for item in state["inventory"])
              and sum(item.get("scroll", False) for item in state["inventory"]) >= 4
              and sum(item.get("book", False) for item in state["inventory"]) >= 2)
    books_read = []
    for name in ("Heal Other", "Telekinesis"):
        before = peer.command("state")
        book = next(item for item in before["inventory"] if item.get("book") and item["spellName"] == name)
        assert peer.command(f"read book {book['id']}")["ok"]
        learned = peer.wait(lambda state: any(spell["source"] == "spell" and spell["name"] == name and spell["level"] > 0
                                              for spell in state["spells"]) and state["async"]["pendingInputs"] == 0)
        assert sum(item.get("book", False) for item in learned["inventory"]) == sum(item.get("book", False) for item in before["inventory"]) - 1
        books_read.append({"name": name, "learned": learned})
    identify = cast_scroll_utility(peer, "Identify")
    unidentified = next(item for item in identify["issued"]["inventory"] if item["magical"] and not item["identified"])
    assert peer.command(f"target item {7 + unidentified['id']}")["queued"]
    identified = peer.wait(lambda state: not state["utility"]["ready"] and state["async"]["pendingInputs"] == 0
                           and any(item["magical"] and item["identified"] for item in state["inventory"]))
    identify["applied"] = identified
    assert not peer.command(f"target item {7 + unidentified['id']}")["ok"]
    host_state = host.command("state")
    wounded_hp = max(1, host_state["maxhp"] // 4)
    assert host.command(f"qa health 0 {wounded_hp}")["fixture"]
    peer.wait(lambda state: any(player["id"] == 0 and player["hp"] == wounded_hp for player in state["players"]))
    heal = cast_scroll_utility(peer, "Heal Other", "spell")
    assert peer.command("target player 0")["queued"]
    heal["applied"] = peer.wait(lambda state: not state["utility"]["ready"] and state["async"]["pendingInputs"] == 0
                               and any(player["id"] == 0 and player["hp"] > wounded_hp for player in state["players"]))
    assert host.command("qa health 0 0")["fixture"]
    peer.wait(lambda state: any(player["id"] == 0 and player["hp"] == 0 for player in state["players"]))
    resurrect = cast_scroll_utility(peer, "Resurrect")
    assert peer.command("target player 0")["queued"]
    resurrect["applied"] = peer.wait(lambda state: not state["utility"]["ready"] and state["async"]["pendingInputs"] == 0
                                    and any(player["id"] == 0 and player["hp"] > 0 for player in state["players"]))
    revived = host.command("state")
    assert revived["hp"] > 0, revived
    assert host.command(f"qa health 0 {revived['maxhp']}")["fixture"]
    return {"fixture": setup, "books_read": books_read, "identify": identify, "heal_other": heal, "resurrect": resurrect}


def check_telekinesis(host, peer):
    state = peer.command("state")
    fixture = host.command(f"qa ground {state['playerId']}")
    assert fixture.get("fixture") and fixture["distance"] > 2, fixture
    peer.wait(lambda state: any(item["id"] == fixture["item"] for item in state["items"]))
    before = peer.command("state")
    identify_id = next(value["id"] for value in before["spells"] if value["source"] == "scroll" and value["name"] == "Identify")
    identify_count = scroll_count(before, identify_id)
    telekinesis = cast_scroll_utility(peer, "Telekinesis", "spell")
    assert peer.command(f"target ground {fixture['item']}")["queued"]
    telekinesis["applied"] = peer.wait(lambda state: not state["utility"]["ready"] and state["async"]["pendingInputs"] == 0
                                      and scroll_count(state, identify_id) == identify_count + 1
                                      and all(item["id"] != fixture["item"] for item in state["items"]))
    telekinesis["fixture"] = fixture
    return telekinesis


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--survivors", action="store_true")
    parser.add_argument("--host-class", choices=("warrior", "rogue", "sorcerer"), default="warrior")
    parser.add_argument("--peer-class", choices=("warrior", "rogue", "sorcerer"), default="warrior")
    parser.add_argument("--deep-utilities", action="store_true", help="Exercise real scroll consumption, identify, heal, resurrect and remote telekinesis with isolated native fixtures")
    parser.add_argument("--utilities", action="store_true", help="Exercise actual class Repair/Recharge cast and authority target with an isolated wear fixture")
    parser.add_argument("--travel", action="store_true", help="Also require both peers follow host into the Cathedral")
    parser.add_argument("--commerce", action="store_true", help="Also walk to Pepin, buy and consume an authoritative potion")
    parser.add_argument("--latency-ms", type=int, default=0, help="One-way TCP proxy delay")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.deep_utilities:
        args.travel = True
    root = args.output_dir or Path(tempfile.mkdtemp(prefix="damnationx-async-native-"))
    root.mkdir(parents=True, exist_ok=True)
    game_port, host_bridge_port, peer_bridge_port = free_port(), free_port(), free_port()
    processes = []
    logs = []
    try:
        def launch(role, bridge_port, join=None):
            folder = root / role
            folder.mkdir()
            (folder / "diablo.ini").write_text(f"[Network]\nPort={game_port}\n[Graphics]\nVSync=0\n")
            command = [str(args.binary.resolve()), "--headless", "--bot-transport", "tcp",
                       "--bot-hero", "AsyncHost" if role == "host" else "AsyncPeer", "--bot-class", args.host_class if role == "host" else args.peer_class,
                       "--data-dir", str(args.data_dir.resolve()), "--save-dir", str(folder), "--config-dir", str(folder)]
            command += ["--bot-host"] if join is None else ["--bot-join", join]
            if args.survivors:
                command += ["--bot-survivors"]
            environment = os.environ.copy()
            environment["DAMNATION_BRIDGE_PORT"] = str(bridge_port)
            log = (folder / "native.log").open("w")
            logs.append(log)
            processes.append(subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=environment))
        launch("host", host_bridge_port)
        host = Bridge(host_bridge_port, process=processes[0], state_file=root / "host-state.json")
        host_initial = host.wait(lambda state: state.get("ingame") and state.get("async", {}).get("enabled") and state["async"]["authority"])
        proxy = DelayProxy(game_port, args.latency_ms / 1000) if args.latency_ms else None
        launch("peer", peer_bridge_port, f"127.0.0.1:{proxy.port if proxy else game_port}")
        peer = Bridge(peer_bridge_port, process=processes[1], state_file=root / "peer-state.json")
        connected = peer.wait(lambda state: state.get("ingame") and state.get("async", {}).get("enabled")
                              and not state["async"]["authority"] and state["async"]["snapshots"] > 2, timeout=60,
                              observe=lambda: host.command("state"))
        assert len(host.wait(lambda state: len(state.get("players", [])) == 2)["players"]) == 2
        # Ask the real native input path to walk several tiles, then require the
        # server and replica agree after the input has been ACKed.
        target = (connected["x"] + 2, connected["y"])
        peer.command(f"walk {target[0]} {target[1]}")
        moved = peer.wait(lambda state: (state.get("x"), state.get("y")) == target
                          and state["async"]["pendingInputs"] == 0, timeout=20)
        observed = host.wait(lambda state: any(player["id"] == moved["playerId"]
                            and (player.get("x"), player.get("y")) == target for player in state.get("players", [])), timeout=10)
        start_tick = observed["async"]["tick"]
        peer.command("stop")
        # Deliberately stall the peer process. The authority must keep ticking.
        processes[1].send_signal(__import__("signal").SIGSTOP)
        time.sleep(0.5)
        advanced = host.command("state")
        processes[1].send_signal(__import__("signal").SIGCONT)
        assert advanced["async"]["tick"] > start_tick + 2, advanced
        recovered = peer.wait(lambda state: state.get("async", {}).get("snapshots", 0) > moved["async"]["snapshots"], timeout=15)
        deep_utility = check_deep_utilities(host, peer) if args.deep_utilities and not args.survivors else None
        utility = check_class_utility(host, peer) if args.utilities and not args.survivors else None
        commerce = check_commerce(peer) if args.commerce and not args.survivors else None
        travel = None
        if args.travel and not args.survivors:
            entrance = next(trigger for trigger in host.command("state")["triggers"] if trigger["destinationLevel"] == 1)
            walk_near(host, entrance)
            host.command(f"walk {entrance['x']} {entrance['y']}")
            host_arrived = host.wait(lambda state: state.get("level") == 1
                                     and state["async"]["pendingInputs"] == 0
                                     and all(player["sameLevel"] and not player["changingLevel"] for player in state["players"]), timeout=30)
            peer_arrived = peer.wait(lambda state: state.get("level") == 1
                                     and state["async"]["latestSnapshot"] >= host_arrived["async"]["tick"]
                                     and all(player["sameLevel"] and not player["changingLevel"] for player in state["players"]), timeout=30)
            assert all(player["sameLevel"] for player in host_arrived["players"]), host_arrived
            travel = {"host": host_arrived, "peer": peer_arrived}
            if deep_utility is not None:
                deep_utility["telekinesis"] = check_telekinesis(host, peer)
        for role in ("host", "peer"):
            native_log = (root / role / "native.log").read_text(errors="replace")
            assert "Remote player validation failed" not in native_log, f"{role}: rejected native actor/item state; see {root / role / 'native.log'}"
        result = {"deep_utility": deep_utility, "utility": utility, "travel": travel, "commerce": commerce, "ok": True, "mode": "survivors" if args.survivors else "campaign", "latency_ms": args.latency_ms,
                  "initial_host": host_initial, "peer_moved": moved, "server_observed": observed,
                  "server_during_peer_stall": advanced, "peer_recovered": recovered}
        (root / "result.json").write_text(json.dumps(result, indent=2))
        print(json.dumps({"ok": True, "output": str(root), "host_tick": advanced["async"]["tick"],
                          "snapshots": recovered["async"]["snapshots"]}))
    finally:
        for process in processes:
            if process.poll() is None:
                try:
                    process.send_signal(__import__("signal").SIGCONT)
                    process.terminate()
                    process.wait(timeout=5)
                except (OSError, subprocess.TimeoutExpired):
                    process.kill()
        for log in logs:
            log.close()


if __name__ == "__main__":
    main()
