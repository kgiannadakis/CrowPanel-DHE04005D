#!/usr/bin/env python3
"""Run actual chat queue/flush methods with bounded String, filesystem and RTOS mocks.

This tests queue and retry behavior, not concurrent scheduling or real flash I/O.
"""
from test_rx_path_parser import ROOT, method, execute_harness


def main():
    source = (ROOT / "examples/crowpanel_p4_lvgl_chat/persistence.cpp").read_text(encoding="utf-8")
    state = source.split("struct PendingChatLine {", 1)[1].split("#endif", 1)[0]
    state = "struct PendingChatLine {" + state
    deadline = source.split("std::atomic<uint32_t> g_chat_flush_deadline_ms", 1)[1].split("void chat_persistence_init()", 1)[0]
    methods = state + "\nstd::atomic<uint32_t> g_chat_flush_deadline_ms" + deadline
    for signature in ("void chat_persistence_init()", "static void chat_retry_pending()",
                      "static bool chat_flush_all_now()", "static bool chat_enqueue_line(",
                      "void chat_append_flush_if_due()", "bool chat_append_flush_sync()"):
        methods += "\n" + method(source, signature)
    harness = (ROOT / "test/test_rx_service/chat_persistence_harness.cpp").read_text(encoding="utf-8")
    execute_harness(harness.replace("// INSERT_PRODUCTION_METHODS", methods))
    print("PASS: production chat queue batching, render-before-queue lock order, overflow flush,")
    print("busy-render retry, deadlines, open-failure retry, partial-write resume and FIFO order.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
