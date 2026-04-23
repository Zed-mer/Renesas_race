"""Tests for project module — .cproject XML parsing."""

import tempfile
from pathlib import Path

from e2studio_mcp.project import (
    find_project_path,
    list_projects,
    parse_cproject,
    read_eclipse_project_name,
    resolve_project,
)


SAMPLE_CPROJECT = """\
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<?fileVersion 4.0.0?><cproject storage_type_id="org.eclipse.cdt.core.XmlProjectDescriptionStorage">
    <storageModule moduleId="org.eclipse.cdt.core.settings">
        <cconfiguration id="test.config.1">
            <storageModule buildSystemId="org.eclipse.cdt.managedbuilder.core.configurationDataProvider" id="test.config.1" moduleId="org.eclipse.cdt.core.settings" name="HardwareDebug">
                <externalSettings/>
                <extensions/>
            </storageModule>
            <storageModule moduleId="com.renesas.cdt.managedbuild.core.toolchainInfo">
                <option id="toolchain.id" value="Renesas_RXC"/>
                <option id="toolchain.version" value="v3.07.00"/>
            </storageModule>
            <storageModule moduleId="cdtBuildSystem" version="4.0.0">
                <configuration artifactExtension="mot" artifactName="${ProjName}" name="HardwareDebug">
                    <folderInfo id="test.1." name="/" resourcePath="">
                        <toolChain id="test.tc.1" name="Renesas CCRX Toolchain">
                            <tool id="test.common" name="Common">
                                <option superClass="com.renesas.cdt.managedbuild.renesas.ccrx.common.option.deviceCommand" value="R5F5651E" valueType="string"/>
                                <option superClass="com.renesas.cdt.managedbuild.renesas.ccrx.common.option.deviceFamily" value="RX651" valueType="string"/>
                                <option superClass="com.renesas.cdt.managedbuild.renesas.ccrx.common.option.isa" value="com.renesas.cdt.managedbuild.renesas.ccrx.common.option.isa.rxv2" valueType="enumerated"/>
                                <option superClass="com.renesas.cdt.managedbuild.renesas.ccrx.common.option.hasFpu" value="TRUE" valueType="string"/>
                            </tool>
                            <tool id="test.dsp" name="DSP Assembler">
                                <option superClass="com.renesas.cdt.managedbuild.renesas.ccrx.dsp.option.endian" value="com.renesas.cdt.managedbuild.renesas.ccrx.dsp.option.endian.big" valueType="enumerated"/>
                            </tool>
                            <tool id="test.compiler" name="Compiler">
                                <option superClass="com.renesas.cdt.managedbuild.renesas.ccrx.compiler.option.include" valueType="includePath">
                                    <listOptionValue builtIn="false" value="${TCINSTALL}/include"/>
                                    <listOptionValue builtIn="false" value="src"/>
                                </option>
                                <option superClass="com.renesas.cdt.managedbuild.renesas.ccrx.compiler.option.define" valueType="definedSymbols">
                                    <listOptionValue builtIn="false" value="DEBUG"/>
                                    <listOptionValue builtIn="false" value="RTOS_ENABLED"/>
                                </option>
                            </tool>
                        </toolChain>
                    </folderInfo>
                </configuration>
            </storageModule>
        </cconfiguration>
    </storageModule>
</cproject>
"""


def test_parse_cproject():
    with tempfile.TemporaryDirectory() as tmpdir:
        proj_dir = Path(tmpdir) / "test-project"
        proj_dir.mkdir()
        cproject = proj_dir / ".cproject"
        cproject.write_text(SAMPLE_CPROJECT, encoding="utf-8")

        cfg = parse_cproject(cproject)
        assert cfg.device == "R5F5651E"
        assert cfg.device_family == "RX651"
        assert cfg.toolchain_id == "Renesas_RXC"
        assert cfg.toolchain_version == "v3.07.00"
        assert cfg.isa == "RXv2"
        assert cfg.has_fpu is True
        assert cfg.endian == "big"
        assert cfg.build_config == "HardwareDebug"
        assert cfg.artifact_extension == "mot"
        assert len(cfg.include_paths) == 2
        assert len(cfg.defines) == 2
        assert "DEBUG" in cfg.defines


def test_list_projects():
    with tempfile.TemporaryDirectory() as tmpdir:
        # Create a fake project
        proj_dir = Path(tmpdir) / "proj1"
        proj_dir.mkdir()
        (proj_dir / ".cproject").write_text(SAMPLE_CPROJECT, encoding="utf-8")

        # Create a non-project directory
        (Path(tmpdir) / "not-a-project").mkdir()

        projects = list_projects(Path(tmpdir))
        assert len(projects) == 1
        assert projects[0]["name"] == "proj1"
        assert projects[0]["device"] == "R5F5651E"


RA_CPROJECT = """\
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<?fileVersion 4.0.0?><cproject storage_type_id="org.eclipse.cdt.core.XmlProjectDescriptionStorage">
    <storageModule moduleId="org.eclipse.cdt.core.settings">
        <cconfiguration id="gnuarm.debug.1">
            <storageModule buildSystemId="org.eclipse.cdt.managedbuilder.core.configurationDataProvider" id="gnuarm.debug.1" moduleId="org.eclipse.cdt.core.settings" name="Debug">
                <externalSettings/>
                <extensions/>
            </storageModule>
            <storageModule moduleId="com.renesas.cdt.managedbuild.core.toolchainInfo">
                <option id="toolchain.id" value="com.renesas.cdt.managedbuild.gnuarm"/>
                <option id="toolchain.version" value="12.2.1"/>
            </storageModule>
            <storageModule moduleId="cdtBuildSystem" version="4.0.0">
                <configuration artifactName="${ProjName}" buildArtefactType="org.eclipse.cdt.build.core.buildArtefactType.exe" name="Debug">
                    <folderInfo id="gnuarm.debug.1." name="/" resourcePath="">
                        <toolChain id="gnuarm.debug.tc" name="GCC ARM Embedded">
                            <builder buildPath="${workspace_loc:/ra_sample}/Debug"/>
                            <tool id="gnuarm.c">
                                <option superClass="ilg.gnuarmeclipse.managedbuild.cross.option.arm.target.family" value="ilg.gnuarmeclipse.managedbuild.cross.option.arm.target.mcpu.cortex-m33" valueType="enumerated"/>
                                <option superClass="ilg.gnuarmeclipse.managedbuild.cross.option.arm.target.fpu.unit" value="ilg.gnuarmeclipse.managedbuild.cross.option.arm.target.fpu.unit.fpv5spd16" valueType="enumerated"/>
                                <option superClass="ilg.gnuarmeclipse.managedbuild.cross.option.c.compiler.include.paths" valueType="includePath">
                                    <listOptionValue builtIn="false" value="&quot;${workspace_loc:/${ProjName}/src}&quot;"/>
                                </option>
                                <option superClass="ilg.gnuarmeclipse.managedbuild.cross.option.c.compiler.defs" valueType="definedSymbols">
                                    <listOptionValue builtIn="false" value="_RENESAS_RA_"/>
                                </option>
                            </tool>
                        </toolChain>
                    </folderInfo>
                </configuration>
            </storageModule>
        </cconfiguration>
    </storageModule>
</cproject>
"""


RA_PROJECT = """\
<?xml version="1.0" encoding="UTF-8"?>
<projectDescription>
    <name>ra-sample-eclipse</name>
</projectDescription>
"""


RA_CONFIGURATION = """\
<?xml version="1.0" encoding="UTF-8"?>
<raConfiguration>
    <option key="CPU" value="RA6M5"/>
    <option key="#TargetName#" value="R7FA6M5BF2CBG"/>
    <option key="#DeviceCommand#" value="R7FA6M5BF"/>
</raConfiguration>
"""


def test_parse_ra_cproject_with_configuration_xml():
    with tempfile.TemporaryDirectory() as tmpdir:
        proj_dir = Path(tmpdir) / "ra_sample"
        proj_dir.mkdir()
        (proj_dir / ".cproject").write_text(RA_CPROJECT, encoding="utf-8")
        (proj_dir / ".project").write_text(RA_PROJECT, encoding="utf-8")
        (proj_dir / "configuration.xml").write_text(RA_CONFIGURATION, encoding="utf-8")

        cfg = parse_cproject(proj_dir / ".cproject", build_config="Debug")
        assert cfg.platform == "ra"
        assert cfg.eclipse_project_name == "ra-sample-eclipse"
        assert cfg.device == "R7FA6M5BF"
        assert cfg.device_family == "RA6M5"
        assert cfg.isa == "Cortex-M33"
        assert cfg.has_fpu is True
        assert cfg.artifact_extension == "elf"
        assert cfg.build_directory.endswith("ra_sample\\Debug")


def test_resolve_project_by_eclipse_name():
    with tempfile.TemporaryDirectory() as tmpdir:
        workspace = Path(tmpdir)
        proj_dir = workspace / "ra_sample"
        proj_dir.mkdir()
        (proj_dir / ".cproject").write_text(RA_CPROJECT, encoding="utf-8")
        (proj_dir / ".project").write_text(RA_PROJECT, encoding="utf-8")
        (proj_dir / "configuration.xml").write_text(RA_CONFIGURATION, encoding="utf-8")
        (proj_dir / "Debug").mkdir()

        assert read_eclipse_project_name(proj_dir) == "ra-sample-eclipse"
        assert find_project_path(workspace, "ra-sample-eclipse") == proj_dir

        resolved = resolve_project(workspace, "ra-sample-eclipse", "Debug")
        assert resolved.path == proj_dir
        assert resolved.eclipse_project_name == "ra-sample-eclipse"
