#!/usr/bin/env python3
"""Bounded intent adapters for ABVx Companion."""

from __future__ import annotations

from dataclasses import dataclass

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
        "sync_time": {"usb_detected": context["usb_detected"]},
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


class RuleBasedIntentAdapter(IntentAdapter):
    def __init__(self):
        super().__init__(name="rule_based")

    def resolve(self, payload, status):
        if not isinstance(payload, dict):
            raise RuntimeError("invalid JSON payload")
        text = payload.get("text", "")
        if not isinstance(text, str):
            raise RuntimeError("intent text must be a string")
        text = " ".join(text.strip().lower().split())
        if not text:
            raise RuntimeError("intent text is empty")
        context = payload.get("context", {})
        if context is not None and not isinstance(context, dict):
            raise RuntimeError("intent context must be an object")
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
            return {
                "status": INTENT_REJECT,
                "intent": None,
                "arguments": {},
                "confidence": 0.0,
                "action_label": "No Matching Command",
                "target_section": "guide",
                "preconditions": {},
                "summary": "This request is outside the Companion command set.",
                "requires_confirmation": False,
                "fallback_reason": "out_of_scope",
                "adapter": self.name,
            }
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
        text = ""
        if isinstance(payload, dict):
            text = str(payload.get("text", "")).strip()
        result["adapter"] = self.name
        result["adapter_mode"] = "stub"
        result["adapter_meta"] = self.descriptor()
        result["adapter_request"] = {
            "model_input": {
                "text": text,
                "context": payload.get("context", {}) if isinstance(payload, dict) else {},
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


def build_intent_adapter(mode):
    normalized = (mode or "").strip().lower()
    if normalized in ("", "rule_based", "rules"):
        return RuleBasedIntentAdapter()
    if normalized in ("needle", "needle_stub", "stub"):
        return NeedleIntentAdapterStub()
    raise RuntimeError(f"unsupported intent adapter: {mode}")
