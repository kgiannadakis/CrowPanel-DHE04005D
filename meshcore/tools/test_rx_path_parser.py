#!/usr/bin/env python3
"""Execute the isolated production packet parser as RV32 code using Unicorn.

Requires unicorn and pyelftools; no hardware, firmware flash or network access.
The SHA/AES/UI/radio paths are outside this test's scope.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# Optional project-local tools, kept under the ignored build directory.
sys.path.insert(0, str(ROOT / ".pio/rx-test-runtime"))


def method(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def execute_harness(harness):
    from elftools.elf.elffile import ELFFile
    from unicorn import Uc, UC_ARCH_RISCV, UC_MODE_RISCV32
    from unicorn.riscv_const import UC_RISCV_REG_SP, UC_RISCV_REG_RA, UC_RISCV_REG_A0

    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    compilers = sorted((core / "packages").glob("toolchain-riscv*/bin/riscv32-esp-elf-g++.exe"))
    if not compilers:
        raise RuntimeError("ESP32 RISC-V compiler not found")
    with tempfile.TemporaryDirectory(prefix="crowpanel-rx-") as temp:
        source = Path(temp) / "parser.cpp"
        elf_path = Path(temp) / "parser.elf"
        source.write_text(harness, encoding="utf-8")
        subprocess.run([str(compilers[0]), "-std=c++17", "-O1", "-march=rv32im", "-mabi=ilp32",
                        "-fno-builtin", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
                        "-nostdlib", "-Wl,-e,run_tests,-Ttext=0x10000,--no-relax",
                        "-I", str(ROOT / "src"), str(source), "-o", str(elf_path)], check=True)
        emu = Uc(UC_ARCH_RISCV, UC_MODE_RISCV32)
        emu.mem_map(0x1000, 0x210000)
        with elf_path.open("rb") as stream:
            elf = ELFFile(stream)
            for segment in elf.iter_segments():
                if segment["p_type"] == "PT_LOAD":
                    emu.mem_write(segment["p_vaddr"], segment.data())
            entry = elf.header["e_entry"]
        emu.reg_write(UC_RISCV_REG_SP, 0x20FFF0)
        emu.reg_write(UC_RISCV_REG_RA, 0x200000)
        emu.emu_start(entry, 0x200000, count=5000000)
        result = emu.reg_read(UC_RISCV_REG_A0)
        if result:
            raise RuntimeError(f"Emulated case failed: {result}")
        from unicorn.riscv_const import UC_RISCV_REG_PC
        if emu.reg_read(UC_RISCV_REG_PC) != 0x200000:
            raise RuntimeError("Emulated test did not finish within instruction budget")


def main():
    dispatcher = (ROOT / "src/Dispatcher.cpp").read_text(encoding="utf-8")
    packet = (ROOT / "src/Packet.cpp").read_text(encoding="utf-8")
    methods = method(dispatcher, "bool Dispatcher::tryParsePacket(") + "\n" + method(packet, "Packet::Packet()")
    harness = (ROOT / "test/test_rx_service/rx_path_parser_harness.cpp").read_text(encoding="utf-8")
    execute_harness(harness.replace("// INSERT_PRODUCTION_METHODS", methods))
    print("PASS: production parser executed for all legal 1/2/3-byte hop counts, four route types,")
    print("truncated paths, reserved/oversize paths, and captured DX jungle/Domibe/RIZZI packets.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
