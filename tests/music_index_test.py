import importlib.util
import tempfile
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "tools" / "abvx_companion.py"
SPEC = importlib.util.spec_from_file_location("abvx_companion", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def _mp3_with_sync(path: Path):
    # Minimal valid-like MP3 payload for has_mp3_sync:
    # - valid ID3-like header so offset is aligned
    # - sync frame present right after the 10-byte header
    payload = bytearray(b"ID3") + bytearray([3, 0, 0, 0, 0, 0, 0])
    payload += bytearray([0xFF, 0xFB, 0x90, 0x00])
    payload += bytearray(128)
    path.write_bytes(payload)


def test_add_music_ascii_index_for_mp3_stem():
    with tempfile.TemporaryDirectory() as root:
        sd = Path(root)
        source = sd / "Альянс На Заре.mp3"
        _mp3_with_sync(source)

        MODULE.add_music(sd, [str(source)])

        index_lines = (sd / "music" / "INDEX.TXT").read_text(encoding="utf-8").splitlines()
        assert len(index_lines) == 1
        assert index_lines[0] == "M001.MP3|Alyans Na Zare"


def test_sync_music_mirror_skips_identical_content_under_different_names():
    with tempfile.TemporaryDirectory() as root:
        root = Path(root)
        source = root / "source"
        mirror = root / "mirror"
        source.mkdir()
        _mp3_with_sync(source / "First.mp3")
        (source / "Second.mp3").write_bytes((source / "First.mp3").read_bytes())

        MODULE.sync_music_mirror(source, mirror)

        prepared = list((mirror / "music").glob("*.MP3"))
        assert len(prepared) == 1
        assert (source / "First.mp3").exists()
        assert (source / "Second.mp3").exists()


if __name__ == "__main__":
    test_add_music_ascii_index_for_mp3_stem()
    test_sync_music_mirror_skips_identical_content_under_different_names()
    print("music index test: OK")
