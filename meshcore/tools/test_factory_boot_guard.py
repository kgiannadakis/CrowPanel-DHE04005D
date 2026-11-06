#!/usr/bin/env python3
"""Execute the production factory preflight/boot guard with mocked flash APIs."""
from test_rx_path_parser import ROOT, method, execute_harness


def main():
    source = (ROOT / "src/crowpanel_dualboot.cpp").read_text(encoding="utf-8")
    methods = method(source, "static bool factory_image_fits(") + "\n" + method(source, 'extern "C" void initVariant()')
    harness = (ROOT / "test/test_rx_service/factory_boot_guard_harness.cpp").read_text(encoding="utf-8")
    execute_harness(harness.replace("// INSERT_PRODUCTION_METHODS", methods))
    print("PASS: production boot guard rejects missing/truncated/oversize/corrupt images before verification,")
    print("preserves OTA on failed validation, and only clears OTA for a valid factory image.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
