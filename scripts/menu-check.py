#!/usr/bin/env python3
"""Check the panel menu over REST, on the device, without a finger.

The menu is driven from the glass, and the glass is on Basti's desk, so the menu's own
commands are reachable over HTTP: `menu_open` is published and may be set, and `menu_press`
takes a row index. Both go through the same open and the same row handler a tap goes through,
which is what makes this a test of the feature rather than of a parallel one.

    python3 scripts/menu-check.py                      # 192.168.2.248, admin/admin
    python3 scripts/menu-check.py --device http://x --user u --password p

It leaves the device as it found it: the play mode and the goal level are read first and put
back at the end, and any session it starts is stopped.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

RESULTS = []


def check(label, ok, detail=""):
    RESULTS.append((label, bool(ok), detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {label}{('  -> ' + detail) if detail else ''}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="http://192.168.2.248")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--password", default="admin")
    parser.add_argument("--settle", type=float, default=2.0, help="seconds to let the device apply a command")
    args = parser.parse_args()
    base = args.device.rstrip("/")
    token = None

    def call(path, data=None, timeout=25):
        request = urllib.request.Request(
            base + path, data=json.dumps(data).encode() if data is not None else None,
            method="POST" if data is not None else "GET")
        request.add_header("Content-Type", "application/json")
        if token:
            request.add_header("Authorization", "Bearer " + token)
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.loads(response.read().decode())
        except urllib.error.HTTPError as error:
            sys.exit(f"{path}: http {error.code}")

    features = call("/rest/features")
    if features.get("security") is not False:
        token = call("/rest/signIn", {"username": args.user, "password": args.password})["access_token"]
    print(f"{base}, firmware {features['firmware_version']}\n")

    def state():
        return call("/rest/trainerState")

    def settings():
        return call("/rest/trainerSettings")

    def post(body):
        return call("/rest/trainerState", body)

    st = state()
    check("the menu fields are in the state", all(k in st for k in ("menu_open", "menu_locked", "menu_press")),
          f"open={st.get('menu_open')} locked={st.get('menu_locked')} press={st.get('menu_press')}")
    check("the menu starts closed", st.get("menu_open") is False)
    check("with nothing running it is not locked", st.get("menu_locked") is False)

    mode0, level0 = settings()["play_mode"], st["goal_level"]
    print(f"\nbefore: play_mode={mode0}, goal_level={level0}")
    try:
        frames = state()["display_frames"]
        post({"menu_open": True})
        time.sleep(args.settle)
        st = state()
        check("the menu opens", st.get("menu_open") is True)
        check("the panel keeps drawing while it is up", st["display_frames"] > frames,
              f"{frames} -> {st['display_frames']}")

        post({"menu_press": 0})
        time.sleep(args.settle)
        mode1 = settings()["play_mode"]
        check("the PLAY row switched the play mode", mode1 != mode0, f"{mode0} -> {mode1}")
        check("the press was consumed", state().get("menu_press") == -1)
        check("the menu stays up after a row press", state().get("menu_open") is True)

        level1 = state()["goal_level"]
        post({"menu_press": 1})
        time.sleep(args.settle)
        level2 = state()["goal_level"]
        check("the LEVEL row stepped the level", level2 != level1, f"{level1} -> {level2}")

        post({"menu_press": 2})
        time.sleep(args.settle)
        check("the CLOSE row closed the menu", state().get("menu_open") is False)

        mode2 = settings()["play_mode"]
        post({"menu_press": 0})
        time.sleep(args.settle)
        check("a row press with no menu open does nothing", settings()["play_mode"] == mode2,
              f"play_mode still {mode2}")

        print("\n--- the lock: a play is running ---")
        post({"session_start": True})
        time.sleep(args.settle + 1)
        st = state()
        check("the session started", st.get("session_active") is True)
        check("the menu reports itself locked", st.get("menu_locked") is True)

        post({"menu_open": True})
        time.sleep(args.settle)
        check("the menu refuses to open during a play", state().get("menu_open") is False)

        mode3, level3 = settings()["play_mode"], state()["goal_level"]
        post({"menu_press": 0})
        time.sleep(args.settle)
        post({"menu_press": 1})
        time.sleep(args.settle)
        check("the play mode cannot change during a play", settings()["play_mode"] == mode3,
              f"{mode3} -> {settings()['play_mode']}")
        check("the level cannot change during a play", state()["goal_level"] == level3,
              f"{level3} -> {state()['goal_level']}")
    finally:
        if state().get("session_active"):
            post({"session_stop": True})
            time.sleep(args.settle)
        call("/rest/trainerSettings", {"play_mode": mode0, "goal_level": level0})
        post({"goal_level": level0})
        time.sleep(args.settle)

    st = state()
    check("the session was stopped again", st.get("session_active") is False)
    check("the menu unlocks when the play ends", st.get("menu_locked") is False)
    post({"menu_open": True})
    time.sleep(args.settle)
    check("and it opens again", state().get("menu_open") is True)
    post({"menu_press": 2})
    time.sleep(args.settle)

    check("the device is left as it was found",
          settings()["play_mode"] == mode0 and state()["goal_level"] == level0,
          f"play_mode={settings()['play_mode']}, goal_level={state()['goal_level']}")

    failed = [r for r in RESULTS if not r[1]]
    print(f"\n==== {len(RESULTS) - len(failed)}/{len(RESULTS)} checks passed ====")
    for label, _, detail in failed:
        print("  FAILED:", label, detail)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
