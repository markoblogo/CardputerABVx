#!/usr/bin/env python3
"""Bounded intent adapters for ABVx Companion."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path

import abvx_companion as core

INTENT_FALLBACK = "fallback"
INTENT_OK = "ok"
INTENT_REJECT = "reject"
INTENT_CONFIRM_REQUIRED = {"sync_time", "sync_music", "sync_books", "sync_voice", "prepare_browser_package"}
INTENT_TOKEN_ALIASES = {
    "sinc": "sync",
    "synk": "sync",
    "synca": "sync",
    "обнови": "sync",
    "обновить": "sync",
    "обновление": "sync",
    "синк": "sync",
    "синкануть": "sync",
    "перегони": "sync",
    "перегнать": "sync",
    "залей": "sync",
    "закинь": "sync",
    "скинь": "sync",
    "copy": "sync",
    "statusa": "status",
    "статус": "status",
    "состояние": "state",
    "проверь": "check",
    "показать": "show",
    "покажи": "show",
    "музыка": "music",
    "музон": "music",
    "трек": "track",
    "треки": "tracks",
    "книга": "book",
    "книги": "books",
    "букс": "books",
    "ридер": "reader",
    "голос": "voice",
    "войс": "voice",
    "запись": "recording",
    "записи": "recordings",
    "рек": "rec",
    "время": "time",
    "часы": "clock",
    "браузер": "browser",
    "избранное": "favorites",
    "страница": "page",
    "страницы": "pages",
    "пакет": "package",
    "карта": "sd",
    "карточка": "sd",
    "диск": "storage",
    "устройство": "device",
    "экспорт": "export",
    "бэкап": "backup",
    "backup": "backup",
    "удали": "delete",
    "очисти": "cleanup",
}


def summarize_intent(intent, arguments):
    if intent == "sd_status":
        return "Show current SD status."
    if intent == "sync_time":
        return "Sync Cardputer time through the current Companion path."
    if intent == "sync_music":
        return "Sync prepared music mirror to the mounted SD card."
    if intent == "sync_books":
        return "Sync prepared books mirror to the mounted SD card."
    if intent == "sync_voice":
        return "Pull voice recordings through the current Companion flow."
    if intent == "prepare_browser_package":
        return "Prepare the browser favorites package."
    return f"Run {intent}."


def intent_target_section(intent):
    mapping = {
        "sd_status": "device",
        "sync_time": "device",
        "sync_music": "content",
        "sync_books": "content",
        "sync_voice": "guide",
        "prepare_browser_package": "content",
    }
    return mapping.get(intent, "device")


def intent_action_label(intent):
    labels = {
        "sd_status": "Load SD Status",
        "sync_time": "Sync Time",
        "sync_music": "Sync Music to SD",
        "sync_books": "Sync Books to SD",
        "sync_voice": "Sync Voice",
        "prepare_browser_package": "Prepare Browser Package",
    }
    return labels.get(intent, intent)


def intent_preconditions(intent, status):
    context = {
        "sd_detected": bool(status.get("sd", {}).get("ready")),
        "usb_detected": bool(status.get("usb_ports")),
        "voice_pending": bool(status.get("backup", {}).get("voice")),
        "browser_package_enabled": False,
    }
    mapping = {
        "sd_status": {},
        "sync_time": {},
        "sync_music": {"sd_detected": context["sd_detected"]},
        "sync_books": {"sd_detected": context["sd_detected"]},
        "sync_voice": {"sd_detected": context["sd_detected"], "voice_pending": context["voice_pending"]},
        "prepare_browser_package": {"browser_package_enabled": context["browser_package_enabled"]},
    }
    return mapping.get(intent, {})


def fallback_payload(reason, confidence, summary, section):
    return {
        "status": INTENT_FALLBACK,
        "intent": None,
        "arguments": {},
        "confidence": confidence,
        "action_label": "Open Companion Panel",
        "target_section": section,
        "preconditions": {},
        "summary": summary,
        "requires_confirmation": False,
        "fallback_reason": reason,
    }


def reject_payload(reason, summary, section="guide"):
    return {
        "status": INTENT_REJECT,
        "intent": None,
        "arguments": {},
        "confidence": 0.0,
        "action_label": "No Matching Command",
        "target_section": section,
        "preconditions": {},
        "summary": summary,
        "requires_confirmation": False,
        "fallback_reason": reason,
    }


def normalize_intent_tokens(text):
    raw_tokens = text.replace("/", " ").replace("-", " ").replace("_", " ").split()
    normalized = set()
    for token in raw_tokens:
        normalized.add(INTENT_TOKEN_ALIASES.get(token, token))
    return normalized


@dataclass
class IntentAdapter:
    name: str

    def resolve(self, payload, status):
        raise NotImplementedError

    def descriptor(self):
        return {
            "name": self.name,
            "mode": "active",
            "transport": "local",
            "schema_version": "intent.v1",
        }

    def _normalize_payload(self, payload):
        if not isinstance(payload, dict):
            raise RuntimeError("invalid JSON payload")
        text = payload.get("text", "")
        if not isinstance(text, str):
            raise RuntimeError("intent text must be a string")
        text = " ".join(text.strip().split())
        if not text:
            raise RuntimeError("intent text is empty")
        context = payload.get("context", {})
        if context is not None and not isinstance(context, dict):
            raise RuntimeError("intent context must be an object")
        return text, context or {}


class RuleBasedIntentAdapter(IntentAdapter):
    def __init__(self):
        super().__init__(name="rule_based")

    def resolve(self, payload, status):
        text, _context = self._normalize_payload(payload)
        text = text.lower()
        tokens = normalize_intent_tokens(text)
        wants_sync = bool(tokens & {"sync", "update", "refresh", "copy", "deploy", "push"})
        wants_status = bool(tokens & {"status", "state", "info", "check", "show"})
        wants_music = bool(tokens & {"music", "track", "tracks", "mp3", "audio", "library"})
        wants_books = bool(tokens & {"book", "books", "reader", "epub", "fb2", "txt"})
        wants_voice = bool(tokens & {"voice", "recording", "recordings", "rec", "audio-notes"})
        wants_time = bool(tokens & {"time", "clock", "rtc"})
        wants_browser = bool(tokens & {"browser", "favorite", "favorites", "package", "page", "pages"})
        wants_card = bool(tokens & {"sd", "card", "storage", "device"})
        if wants_status and (wants_card or not (wants_music or wants_books or wants_voice or wants_time or wants_browser)):
            intent, arguments, confidence = "sd_status", {}, 0.99
        elif wants_time and (wants_sync or "set" in tokens or "show" in tokens):
            intent, arguments, confidence = "sync_time", {"target": "device"}, 0.92
        elif wants_music and wants_sync:
            intent, arguments, confidence = "sync_music", {"target": "sd"}, 0.93
        elif wants_books and wants_sync:
            intent, arguments, confidence = "sync_books", {"target": "sd"}, 0.93
        elif wants_voice and (wants_sync or "pull" in tokens or "export" in tokens or "backup" in tokens):
            delete_after = bool(tokens & {"delete", "cleanup", "clean", "remove"})
            intent, arguments, confidence = "sync_voice", {"delete_after": delete_after}, 0.87
        elif wants_browser and ("prepare" in tokens or "package" in tokens or "build" in tokens):
            intent, arguments, confidence = "prepare_browser_package", {"profile": "favorites"}, 0.75
        else:
            result = reject_payload("out_of_scope", "This request is outside the Companion command set.")
            result["adapter"] = self.name
            return result
        arguments = core.validate_intent_arguments(intent, arguments)
        preconditions = intent_preconditions(intent, status)
        if intent in ("sync_music", "sync_books") and not preconditions.get("sd_detected"):
            payload = fallback_payload("missing_sd", confidence, "Mount the SD card, then use the sync panel.", "device")
            payload["adapter"] = self.name
            return payload
        if intent == "sync_voice" and not preconditions.get("voice_pending"):
            payload = fallback_payload("voice_empty", confidence, "No voice items are ready for sync in the current Companion context.", "guide")
            payload["adapter"] = self.name
            return payload
        if intent == "prepare_browser_package" and not preconditions.get("browser_package_enabled"):
            payload = fallback_payload("feature_disabled", confidence, "Browser package preparation is not enabled in this build.", "content")
            payload["adapter"] = self.name
            return payload
        return {
            "status": INTENT_OK,
            "intent": intent,
            "arguments": arguments,
            "confidence": confidence,
            "action_label": intent_action_label(intent),
            "target_section": intent_target_section(intent),
            "preconditions": preconditions,
            "summary": summarize_intent(intent, arguments),
            "requires_confirmation": intent in INTENT_CONFIRM_REQUIRED,
            "fallback_reason": None,
            "adapter": self.name,
        }


class NeedleIntentAdapterStub(IntentAdapter):
    def __init__(self):
        super().__init__(name="needle_stub")
        self.fallback = RuleBasedIntentAdapter()

    def resolve(self, payload, status):
        result = self.fallback.resolve(payload, status)
        text, context = self._normalize_payload(payload)
        result["adapter"] = self.name
        result["adapter_mode"] = "stub"
        result["adapter_meta"] = self.descriptor()
        result["adapter_request"] = {
            "model_input": {
                "text": text,
                "context": context,
                "allowed_intents": list(core.INTENT_NAMES),
            }
        }
        result["adapter_response"] = {
            "intent": result.get("intent"),
            "arguments": result.get("arguments", {}),
            "confidence": result.get("confidence", 0.0),
            "status": result.get("status"),
        }
        return result

    def descriptor(self):
        descriptor = super().descriptor()
        descriptor["mode"] = "stub"
        descriptor["backend_family"] = "needle"
        return descriptor


class NeedleIntentAdapter(IntentAdapter):
    def __init__(self):
        super().__init__(name="needle")
        self._needle = None
        self._agent = None
        self._init_error = None
        self._system_facts_cache = None
        self._threshold = float(os.environ.get("ABVX_INTENT_CONFIDENCE", "0.75"))
        self._weights = os.environ.get("ABVX_INTENT_NEEDLE_WEIGHTS")
        self._tool_index_path = os.environ.get(
            "ABVX_INTENT_NEEDLE_TOOL_INDEX",
            str(Path.home() / "Library/Application Support" / "ABVx Companion" / "needle-tools.idx"),
        )

    def descriptor(self):
        descriptor = super().descriptor()
        descriptor["backend_family"] = "needle"
        descriptor["confidence_threshold"] = self._threshold
        descriptor["weights"] = self._weights or "default"
        descriptor["tool_index_path"] = self._tool_index_path
        if self._init_error:
            descriptor["mode"] = "degraded"
            descriptor["error"] = self._init_error
        return descriptor

    def _tool_catalog(self):
        descriptions = {
            "sd_status": "Show current ABVx SD status and storage availability.",
            "sync_time": "Sync Cardputer time through the Companion time sync flow.",
            "sync_music": "Sync the prepared music mirror to the mounted SD card.",
            "sync_books": "Sync the prepared books mirror to the mounted SD card.",
            "sync_voice": "Pull voice recordings through the Companion backup flow.",
            "prepare_browser_package": "Prepare the browser favorites package when available.",
        }
        tools = []
        for intent in core.INTENT_NAMES:
            fields = core.intent_schema(intent)
            properties = {}
            required = []
            for name, rule in fields.items():
                if rule is bool:
                    properties[name] = {"type": "boolean"}
                elif isinstance(rule, tuple):
                    properties[name] = {"type": "string", "enum": list(rule)}
                else:
                    continue
                required.append(name)
            tools.append({
                "name": intent,
                "description": descriptions[intent],
                "parameters": {
                    "type": "object",
                    "properties": properties,
                    "required": required,
                    "additionalProperties": False,
                },
            })
        return tools

    def _system_facts(self, status):
        return "; ".join((
            f"sd_detected: {bool(status.get('sd', {}).get('ready'))}",
            f"usb_detected: {bool(status.get('usb_ports'))}",
            f"voice_pending: {bool(status.get('backup', {}).get('voice'))}",
            "assistant: ABVx Companion intent router",
            "Only choose one declared tool",
            "Return empty call when no declared tool matches",
            "Do not guess missing arguments",
        ))

    def _ensure_agent(self, status):
        system_facts = self._system_facts(status)
        if self._agent is not None and self._system_facts_cache == system_facts:
            try:
                self._agent.reset()
            except Exception:
                pass
            return self._agent
        if self._agent is not None and self._system_facts_cache != system_facts:
            self._agent = None
        try:
            import needle  # type: ignore
        except Exception as exc:
            self._init_error = str(exc)
            raise RuntimeError(f"needle runtime unavailable: {exc}")
        self._needle = needle
        kwargs = {"tools": self._tool_catalog(), "system": system_facts}
        if self._weights:
            weights_path = Path(self._weights).expanduser()
            if not weights_path.is_file():
                raise RuntimeError(f"needle weights not found: {weights_path}")
            kwargs["weights"] = str(weights_path)
        if self._tool_index_path:
            tool_index = Path(self._tool_index_path).expanduser()
            tool_index.parent.mkdir(parents=True, exist_ok=True)
            kwargs["tool_index_path"] = str(tool_index)
        self._agent = needle.Needle(**kwargs)
        self._system_facts_cache = system_facts
        self._init_error = None
        return self._agent

    def resolve(self, payload, status):
        text, context = self._normalize_payload(payload)
        try:
            agent = self._ensure_agent(status)
            response = agent.complete(text, max_new_tokens=256)
        except Exception as exc:
            result = fallback_payload("backend_unavailable", 0.0, f"Needle backend unavailable: {exc}", "guide")
            result["adapter"] = self.name
            result["adapter_meta"] = self.descriptor()
            return result

        calls = response.get("function_calls") or []
        confidence = float(response.get("confidence", 0.0) or 0.0)
        result = {
            "adapter": self.name,
            "adapter_mode": "runtime",
            "adapter_meta": self.descriptor(),
            "adapter_request": {
                "model_input": {
                    "text": text,
                    "context": context,
                    "allowed_intents": list(core.INTENT_NAMES),
                }
            },
            "adapter_response": {
                "type": response.get("type"),
                "success": response.get("success"),
                "error": response.get("error"),
                "error_code": response.get("error_code"),
                "function_calls": calls,
                "reasoning": response.get("reasoning"),
                "confidence": confidence,
                "prefill_tps": response.get("prefill_tps"),
                "decode_tps": response.get("decode_tps"),
                "peak_ram_mb": response.get("peak_ram_mb"),
            },
        }
        if not calls:
            result.update(reject_payload("out_of_scope", "This request is outside the Companion command set."))
            return result
        call = calls[0]
        intent = call.get("name")
        arguments = call.get("arguments", {})
        if intent not in core.INTENT_NAMES:
            result.update(reject_payload("out_of_scope", "Needle selected an unsupported Companion action."))
            return result
        try:
            arguments = core.validate_intent_arguments(intent, arguments)
        except Exception as exc:
            result.update(fallback_payload("invalid_arguments", confidence, f"Needle produced invalid arguments: {exc}", intent_target_section(intent)))
            return result
        preconditions = intent_preconditions(intent, status)
        if confidence < self._threshold:
            result.update(fallback_payload("low_confidence", confidence, "Confidence is below the Companion threshold. Use the suggested panel or confirm manually.", intent_target_section(intent)))
            return result
        if intent in ("sync_music", "sync_books") and not preconditions.get("sd_detected"):
            result.update(fallback_payload("missing_sd", confidence, "Mount the SD card, then use the sync panel.", "device"))
            return result
        if intent == "sync_voice" and not preconditions.get("voice_pending"):
            result.update(fallback_payload("voice_empty", confidence, "No voice items are ready for sync in the current Companion context.", "guide"))
            return result
        if intent == "prepare_browser_package" and not preconditions.get("browser_package_enabled"):
            result.update(fallback_payload("feature_disabled", confidence, "Browser package preparation is not enabled in this build.", "content"))
            return result
        result.update({
            "status": INTENT_OK,
            "intent": intent,
            "arguments": arguments,
            "confidence": confidence,
            "action_label": intent_action_label(intent),
            "target_section": intent_target_section(intent),
            "preconditions": preconditions,
            "summary": summarize_intent(intent, arguments),
            "requires_confirmation": intent in INTENT_CONFIRM_REQUIRED,
            "fallback_reason": None,
        })
        return result


def build_intent_adapter(mode):
    normalized = (mode or "").strip().lower()
    if normalized in ("", "rule_based", "rules"):
        return RuleBasedIntentAdapter()
    if normalized in ("needle_stub", "stub"):
        return NeedleIntentAdapterStub()
    if normalized in ("needle", "runtime"):
        return NeedleIntentAdapter()
    raise RuntimeError(f"unsupported intent adapter: {mode}")
