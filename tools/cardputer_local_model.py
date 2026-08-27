#!/usr/bin/env python3
"""Query the Mac-local MPS worker from the Cardputer Companion workspace."""

import argparse
import json
import os
from pathlib import Path
from urllib.request import Request, urlopen


DEFAULT_URL = os.environ.get("CARDPUTER_LOCAL_MODEL_URL", "http://127.0.0.1:8766")


def request(method: str, path: str, payload: dict | None = None) -> dict:
    data = json.dumps(payload).encode() if payload is not None else None
    headers = {"Content-Type": "application/json"} if data else {}
    with urlopen(Request(f"{DEFAULT_URL}{path}", data=data, headers=headers, method=method), timeout=45) as response:
        return json.loads(response.read().decode())


def main() -> int:
    global DEFAULT_URL
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=DEFAULT_URL)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("health")
    answer = sub.add_parser("answer")
    answer.add_argument("--file", required=True)
    args = parser.parse_args()
    DEFAULT_URL = args.url.rstrip("/")
    if args.command == "health":
        result = request("GET", "/health")
    else:
        payload = json.loads(Path(args.file).read_text(encoding="utf-8"))
        payload["project"] = "cardputer"
        result = request("POST", "/v1/answer", payload)
        evidence = result.get("evidence", {})
        if evidence.get("context_mode") != "explicit_only" or evidence.get("live_proof") is not False:
            raise SystemExit("ERROR: local model receipt violated read-only explicit-context contract")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
