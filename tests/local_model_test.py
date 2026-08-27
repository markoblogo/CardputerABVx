import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import abvx_companion_app as app


def test_local_model_receipt_contract():
    original = app.urllib.request.urlopen

    class Response:
        def __enter__(self): return self
        def __exit__(self, *_): return False
        def read(self):
            return json.dumps({"ok": True, "evidence": {"context_mode": "explicit_only", "live_proof": False}, "status": "ABSTAINED"}).encode()

    app.urllib.request.urlopen = lambda *_args, **_kwargs: Response()
    try:
        result = app.local_model_answer({"question": "What is proven?", "context": []})
        assert result["status"] == "ABSTAINED"
    finally:
        app.urllib.request.urlopen = original


if __name__ == "__main__":
    test_local_model_receipt_contract()
    print("local model contract test: OK")
