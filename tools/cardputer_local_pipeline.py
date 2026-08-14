#!/usr/bin/env python3
"""Cardputer Local Pipeline

Workflow helper for host-side content staging:
1) keep human-friendly sources under Cardputer Local/Music Source and Books Source
2) build prepared runtime mirror under Cardputer Local/Exports/CardP SD Mirror
3) optionally deploy mirror sections directly to mounted SD
"""

from __future__ import annotations

import argparse
from pathlib import Path

import abvx_companion as core


def _section_path(root: Path, section: str) -> Path:
    if section == "music":
        return core.default_music_source(root)
    if section == "books":
        return core.default_books_source(root)
    raise ValueError(f"unknown section: {section}")


def cmd_init(args: argparse.Namespace) -> None:
    root = core.local_root(args.root)
    (root / "Music Source").mkdir(parents=True, exist_ok=True)
    (root / "Books Source").mkdir(parents=True, exist_ok=True)
    (root / "Backups").mkdir(parents=True, exist_ok=True)
    (root / "Backups" / "Notes").mkdir(parents=True, exist_ok=True)
    (root / "Backups" / "Recordings").mkdir(parents=True, exist_ok=True)
    (root / "Exports" / "CardP SD Mirror").mkdir(parents=True, exist_ok=True)
    (root / "Exports" / "CardP SD Mirror" / "music").mkdir(parents=True, exist_ok=True)
    (root / "Exports" / "CardP SD Mirror" / "books").mkdir(parents=True, exist_ok=True)
    core.ensure_layout(root / "Exports" / "CardP SD Mirror")
    print(f"OK local pipeline ready\nroot={root}")
    print("Music Source:", root / "Music Source")
    print("Books Source:", root / "Books Source")
    print("Mirror:", root / "Exports" / "CardP SD Mirror")


def cmd_sync_music(args: argparse.Namespace) -> None:
    root = core.local_root(args.root)
    source = _section_path(root, "music")
    mirror = Path(args.mirror).expanduser().resolve() if args.mirror else core.default_sd_mirror(root)
    core.sync_music_mirror(source_dir=str(source), mirror_root=str(mirror))
    if args.deploy:
        sd = core.resolve_sd(args.sd, allow_empty=False)
        core.deploy_mirror_to_sd(sd, mirror, "music")
        print(f"OK deploy music mirror={mirror} -> {sd}")


def cmd_sync_books(args: argparse.Namespace) -> None:
    root = core.local_root(args.root)
    source = _section_path(root, "books")
    mirror = Path(args.mirror).expanduser().resolve() if args.mirror else core.default_sd_mirror(root)
    core.sync_books_mirror(source_dir=str(source), mirror_root=str(mirror))
    if args.deploy:
        sd = core.resolve_sd(args.sd, allow_empty=False)
        core.deploy_mirror_to_sd(sd, mirror, "books")
        print(f"OK deploy books mirror={mirror} -> {sd}")


def cmd_sync_all(args: argparse.Namespace) -> None:
    cmd_sync_music(args)
    cmd_sync_books(args)


def cmd_status(args: argparse.Namespace) -> None:
    root = core.local_root(args.root)
    music = _section_path(root, "music")
    books = _section_path(root, "books")
    mirror = core.default_sd_mirror(root)
    def count(path: Path, suffix: str) -> int:
        return sum(1 for p in path.iterdir() if p.is_file() and p.suffix.lower() == suffix) if path.is_dir() else 0
    print(f"root={root}")
    print(f"music_src={count(music, '.mp3')} mp3")
    print(f"books_src={count(books, '.txt') + count(books, '.epub') + count(books, '.fb2') + count(books, '.htm') + count(books, '.html')} files")
    print(f"mirror_music={count(mirror / 'music', '.mp3')} prepared mp3")
    print(f"mirror_books={count(mirror / 'books', '.txt')} prepared txt")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Host-side Cardputer local pipeline")
    parser.set_defaults(func=None)
    parser.add_argument("--root", default=str(core.DEFAULT_LOCAL_ROOT), help="Cardputer Local folder root")
    sub = parser.add_subparsers(dest="command", required=True)

    init = sub.add_parser("init", help="create default local folder contract")
    init.set_defaults(func=cmd_init)

    music = sub.add_parser("sync-music", help="rebuild music mirror")
    music.add_argument("--mirror", help="override mirror path")
    music.add_argument("--deploy", action="store_true", help="copy mirror section to SD")
    music.add_argument("--sd", help="explicit SD mountpoint")
    music.set_defaults(func=cmd_sync_music)

    books = sub.add_parser("sync-books", help="rebuild books mirror")
    books.add_argument("--mirror", help="override mirror path")
    books.add_argument("--deploy", action="store_true", help="copy mirror section to SD")
    books.add_argument("--sd", help="explicit SD mountpoint")
    books.set_defaults(func=cmd_sync_books)

    all_cmd = sub.add_parser("sync-all", help="rebuild all mirrors")
    all_cmd.add_argument("--mirror", help="override mirror path")
    all_cmd.add_argument("--deploy", action="store_true", help="copy both sections to SD")
    all_cmd.add_argument("--sd", help="explicit SD mountpoint")
    all_cmd.set_defaults(func=cmd_sync_all)

    status = sub.add_parser("status", help="print source and mirror counts")
    status.set_defaults(func=cmd_status)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    if not args.func:
        parser.print_help()
        return 1
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
