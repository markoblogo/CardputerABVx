import importlib.util
import sys
import types
from pathlib import Path


CORE_PATH = Path(__file__).parents[1] / "tools" / "abvx_companion.py"
CORE_SPEC = importlib.util.spec_from_file_location("abvx_companion", CORE_PATH)
CORE_MODULE = importlib.util.module_from_spec(CORE_SPEC)
sys.modules["abvx_companion"] = CORE_MODULE
CORE_SPEC.loader.exec_module(CORE_MODULE)

MODULE_PATH = Path(__file__).parents[1] / "tools" / "needle_intent_adapter.py"
SPEC = importlib.util.spec_from_file_location("needle_intent_adapter", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules["needle_intent_adapter"] = MODULE
SPEC.loader.exec_module(MODULE)


def _status(sd_ready=True, voice_count=1):
    return {
        "sd": {"ready": sd_ready},
        "usb_ports": [],
        "backup": {"voice": voice_count},
    }


def test_rule_based_music_resolve_contract():
    adapter = MODULE.build_intent_adapter("rule_based")
    resolved = adapter.resolve({"text": "sync music to sd", "context": {}}, _status())
    assert resolved["status"] == "ok"
    assert resolved["intent"] == "sync_music"
    assert resolved["arguments"] == {"target": "sd"}
    assert resolved["requires_confirmation"] is True
    assert resolved["adapter"] == "rule_based"


def test_rule_based_voice_empty_fallback():
    adapter = MODULE.build_intent_adapter("rule_based")
    resolved = adapter.resolve({"text": "sync voice", "context": {}}, _status(voice_count=0))
    assert resolved["status"] == "fallback"
    assert resolved["fallback_reason"] == "voice_empty"
    assert resolved["target_section"] == "guide"


def test_needle_stub_envelope():
    adapter = MODULE.build_intent_adapter("needle_stub")
    resolved = adapter.resolve({"text": "show sd status", "context": {"sd_detected": True}}, _status())
    assert resolved["status"] == "ok"
    assert resolved["adapter"] == "needle_stub"
    assert resolved["adapter_mode"] == "stub"
    assert resolved["adapter_meta"]["backend_family"] == "needle"
    assert resolved["adapter_request"]["model_input"]["allowed_intents"] == list(MODULE.core.INTENT_NAMES)
    assert resolved["adapter_response"]["intent"] == "sd_status"


def test_needle_runtime_envelope_with_fake_module():
    fake_module = types.SimpleNamespace()

    class FakeAgent:
        def __init__(self, tools=None, system=None, weights=None):
            self.tools = tools or []
            self.system = system or []
            self.weights = weights

        def reset(self):
            return None

        def complete(self, text, max_new_tokens=256):
            return {
                "type": "call",
                "success": True,
                "error": None,
                "error_code": None,
                "function_calls": [{"name": "sync_music", "arguments": {"target": "sd"}}],
                "reasoning": "'music' -> sync_music",
                "confidence": 0.95,
                "prefill_tps": 1234.0,
                "decode_tps": 456.0,
                "peak_ram_mb": 28.0,
            }

    fake_module.Needle = FakeAgent
    sys.modules["needle"] = fake_module
    adapter = MODULE.build_intent_adapter("needle")
    resolved = adapter.resolve({"text": "sync music to sd", "context": {}}, _status())
    assert resolved["status"] == "ok"
    assert resolved["adapter"] == "needle"
    assert resolved["adapter_mode"] == "runtime"
    assert resolved["adapter_response"]["function_calls"][0]["name"] == "sync_music"
    assert resolved["arguments"] == {"target": "sd"}
    sys.modules.pop("needle", None)


if __name__ == "__main__":
    test_rule_based_music_resolve_contract()
    test_rule_based_voice_empty_fallback()
    test_needle_stub_envelope()
    test_needle_runtime_envelope_with_fake_module()
    print("needle intent adapter test: OK")
