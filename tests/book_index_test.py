import importlib.util
import tempfile
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "tools" / "abvx_companion.py"
SPEC = importlib.util.spec_from_file_location("abvx_companion", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_book_index_ascii_fallback():
    with tempfile.TemporaryDirectory() as root:
        books = Path(root) / "books"
        books.mkdir()
        MODULE.write_book_index(books, "B0001.TXT", "The History of Mr. Polly by H. G. Wells _ Project Gutenberg", "html", "Author")
        MODULE.write_book_index(books, "B0002.TXT", "Привет мир", "txt", "Author")
        lines = (books / "BOOKS.IDX").read_text(encoding="utf-8").splitlines()
        assert lines == [
            "B0001.TXT|The History of Mr. Polly by H. G. Wells _ Project Gutenberg|html|Author",
            "B0002.TXT|Privet mir|txt|Author",
        ]


if __name__ == "__main__":
    test_book_index_ascii_fallback()
    print("book index test: OK")
