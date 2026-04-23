"""Tests for RA launch parsing and ELF memory analysis helpers."""

from pathlib import Path

from e2studio_mcp.config import Config, ToolchainConfig
from e2studio_mcp.ra import (
    _pyocd_load_command,
    parse_launch_file,
    parse_memory_regions,
    parse_objdump_sections,
)


SAMPLE_LAUNCH = """\
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<launchConfiguration type="ilg.gnumcueclipse.debug.gdbjtag.pyocd.launchConfigurationType">
    <stringAttribute key="ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerExecutable" value="${pyocd_path}/${pyocd_executable}"/>
    <stringAttribute key="ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerOther" value="--config C:\\Users\\user\\RA_PYOCD\\pyocd.yaml --connect under-reset"/>
    <stringAttribute key="ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerTargetName" value="R7FA6M5BF"/>
    <stringAttribute key="ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerBoardId" value="0001A0000001"/>
    <intAttribute key="ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerGdbPortNumber" value="3333"/>
    <intAttribute key="ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerTelnetPortNumber" value="4444"/>
    <intAttribute key="ilg.gnumcueclipse.debug.gdbjtag.pyocd.gdbServerBusSpeed" value="8000000"/>
    <booleanAttribute key="ilg.gnumcueclipse.debug.gdbjtag.pyocd.doContinue" value="true"/>
    <stringAttribute key="org.eclipse.cdt.launch.PROGRAM_NAME" value="Debug/demo.elf"/>
    <stringAttribute key="org.eclipse.cdt.launch.PROJECT_ATTR" value="demo"/>
</launchConfiguration>
"""


SAMPLE_MEMORY_REGIONS = """\
RAM_START = 0x20000000;
RAM_LENGTH = 0x80000;
FLASH_START = 0x00000000;
FLASH_LENGTH = 0x100000;
DATA_FLASH_START = 0x08000000;
DATA_FLASH_LENGTH = 0x2000;
OPTION_SETTING_START = 0x0100A100;
OPTION_SETTING_LENGTH = 0x100;
OPTION_SETTING_S_START = 0x0100A200;
OPTION_SETTING_S_LENGTH = 0x100;
"""


SAMPLE_OBJDUMP = """\
Sections:
Idx Name          Size      VMA       LMA       File off  Algn
  0 .text         00001000  00000000  00000000  00010000  2**3
                  CONTENTS, ALLOC, LOAD, READONLY, CODE
  1 .data         00000020  20000000  00001000  00020000  2**2
                  CONTENTS, ALLOC, LOAD, DATA
  2 .bss          00000040  20000020  20000020  00020020  2**2
                  ALLOC
  3 .option_setting_ofs 00000014  0100A100  0100A100  00021000  2**2
                  CONTENTS, ALLOC, LOAD, READONLY, DATA
  4 .debug_info   00000080  00000000  00000000  00022000  2**0
                  CONTENTS, READONLY, DEBUGGING, OCTETS
"""


def test_parse_ra_pyocd_launch(tmp_path: Path):
    launch = tmp_path / "demo.launch"
    launch.write_text(SAMPLE_LAUNCH, encoding="utf-8")

    cfg = parse_launch_file(launch)
    assert cfg.backend == "pyocd"
    assert cfg.project_name == "demo"
    assert cfg.program_name == "Debug/demo.elf"
    assert cfg.gdb_server_target_name == "R7FA6M5BF"
    assert cfg.gdb_server_board_id == "0001A0000001"
    assert cfg.bus_speed == 8000000
    assert cfg.do_continue is True


def test_parse_memory_regions(tmp_path: Path):
    memory_regions = tmp_path / "memory_regions.ld"
    memory_regions.write_text(SAMPLE_MEMORY_REGIONS, encoding="utf-8")

    regions = parse_memory_regions(memory_regions)
    assert regions["FLASH"] == (0x00000000, 0x100000)
    assert regions["RAM"] == (0x20000000, 0x80000)
    assert regions["DATA_FLASH"] == (0x08000000, 0x2000)


def test_parse_objdump_sections():
    regions = {
        "FLASH": (0x00000000, 0x100000),
        "RAM": (0x20000000, 0x80000),
        "DATA_FLASH": (0x08000000, 0x2000),
        "OPTION_SETTING": (0x0100A100, 0x100),
        "OPTION_SETTING_S": (0x0100A200, 0x100),
    }
    summary = parse_objdump_sections(SAMPLE_OBJDUMP, regions)
    assert summary.total_rom == 0x1034
    assert summary.total_ram == 0x60
    assert summary.total_data_flash == 0
    assert len(summary.sections) == 4


def test_build_pyocd_load_command(tmp_path: Path):
    launch = tmp_path / "demo.launch"
    launch.write_text(SAMPLE_LAUNCH, encoding="utf-8")
    cfg = parse_launch_file(launch)

    config = Config(
        platform="ra",
        toolchain=ToolchainConfig(pyocd_path=str(tmp_path)),
    )
    (tmp_path / "pyocd.exe").write_text("", encoding="utf-8")

    image = tmp_path / "demo.elf"
    image.write_text("", encoding="utf-8")

    cmd = _pyocd_load_command(config, cfg, image)
    assert cmd[0].endswith("pyocd.exe")
    assert "--config" in cmd
    assert "-u" in cmd
    assert "-t" in cmd
    assert str(image) == cmd[-1]
