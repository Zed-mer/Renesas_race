"""Assign the adapter-panel RESET net to CPU1 P411 through FSP 6.4."""

from pathlib import Path
import xml.etree.ElementTree as ET

output = open(argv[1], "w", encoding="utf-8")


def emit(message):
    output.write(str(message) + "\n")
    output.flush()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def xml_pin_configuration(project_path, root_name):
    source = Path(project_path) / (
        "solution.xml" if root_name == "raSolution" else "configuration.xml"
    )
    root = ET.parse(source).getroot()
    require(root.tag == root_name, "Unexpected XML root in " + str(source))
    pin_nodes = root.findall("./raPinConfiguration")
    require(len(pin_nodes) == 1, "Expected one raPinConfiguration in " + str(source))
    profiles = pin_nodes[0].findall("./pincfg")
    names = [profile.get("name", "") for profile in profiles]
    require(
        len(names) == len(set(names)),
        "Duplicate pin profiles in " + str(source),
    )
    return root, pin_nodes[0], profiles


def option_value(root, key):
    for option in root.findall("./generalSettings/option"):
        if option.get("key") == key:
            return option.get("value", "")
    return ""


def profile_by_name(profiles, name):
    matches = [profile for profile in profiles if profile.get("name") == name]
    require(len(matches) == 1, "Expected one pin profile named " + name)
    return matches[0]


def profile_has_panel_reset(profile):
    settings = {
        setting.get("configurationId", ""): setting.get("altId", "")
        for setting in profile.findall("./configSetting")
    }
    return (
        settings.get("p411") == "p411.output.low"
        and settings.get("p411.gpio_mode")
        == "p411.gpio_mode.gpio_mode_out.low"
        and not any("dsi_te" in value.lower() for value in settings.values())
    )


def expected_profile_selection(profiles, selected_name):
    for profile in profiles:
        selected = profile.get("name") == selected_name
        if (profile.get("active") == "true") != selected:
            return False
        if (profile.get("selected") == "true") != selected:
            return False
        expected_symbol = "g_bsp_pin_cfg" if selected else ""
        if profile.get("symbol", "") != expected_symbol:
            return False
    return True


def solution_is_configured(solution_path):
    root, pin_node, profiles = xml_pin_configuration(solution_path, "raSolution")
    require(
        option_value(root, "#TargetName#") == "R7KA8P1KFLCAC",
        "Unexpected Solution target",
    )
    require(option_value(root, "#FSPVersion#") == "6.4.0", "Unexpected FSP version")
    symbols = [
        node.get("value", "")
        for node in pin_node.findall("./symbolicName")
        if node.get("propertyId") == "p411.symbolic_name"
    ]
    target = profile_by_name(profiles, "RA8P1_CPKHMI.pincfg")
    return (
        symbols == ["PANEL_RESET"]
        and expected_profile_selection(profiles, "RA8P1_CPKHMI.pincfg")
        and profile_has_panel_reset(target)
    )


def child_is_configured(project_path, profile_name, owner):
    root, pin_node, profiles = xml_pin_configuration(project_path, "raConfiguration")
    require(
        option_value(root, "#TargetName#") == "R7KA8P1KFLCAC",
        "Unexpected child target",
    )
    require(option_value(root, "#FSPVersion#") == "6.4.0", "Unexpected FSP version")
    target = profile_by_name(profiles, profile_name)
    symbols = [
        node.get("value", "")
        for node in pin_node.findall("./symbolicName")
        if node.get("propertyId") == "p411.symbolic_name"
    ]
    if owner:
        return (
            symbols == ["PANEL_RESET"]
            and expected_profile_selection(profiles, profile_name)
            and profile_has_panel_reset(target)
        )
    active_settings = {
        setting.get("configurationId", ""): setting.get("altId", "")
        for setting in target.findall("./configSetting")
    }
    return (
        not symbols
        and expected_profile_selection(profiles, profile_name)
        and not any(key == "p411" or key.startswith("p411.") for key in active_settings)
        and not any("dsi_te" in value.lower() for value in active_settings.values())
    )


def pin_configuration_by_name(configurations, name):
    for configuration in configurations:
        if str(configuration.getName()) == name:
            return configuration
    raise RuntimeError("Pin configuration not found: " + name)


def configure_child(configuration, project_name, profile_name, owner):
    pin_configurator = jvm.com.renesas.tools.pinconfigurator.PinConfigurator
    pin_set = (
        pin_configurator.getInstance(project_name)
        .getModelWrapper()
        .getPinConfigSet()
    )
    target_model = pin_set.getPinConfig(profile_name)
    require(target_model is not None, "Pin model not found: " + profile_name)

    if owner:
        pin_set.activePinConfig(target_model)
        pin_set.selectPinConfig(target_model)
        target_model.setSymbol("g_bsp_pin_cfg")
        for pin_model in pin_set.getPinConfigs():
            if (
                str(pin_model.getName()) != profile_name
                and str(pin_model.getSymbol()) == "g_bsp_pin_cfg"
            ):
                pin_model.setSymbol("")

    for pin_configuration in configuration.getPinConfigurations():
        p411 = pin_configuration.getComponent("p411")
        if p411 is None:
            continue
        p411.getConfig("p411.gpio_mode").setValue(
            "p411.gpio_mode.gpio_mode_out.low"
            if owner and str(pin_configuration.getName()) == profile_name
            else "p411.gpio_mode.disabled"
        )

    target_configuration = configuration.getPinConfiguration(profile_name)
    target_configuration.getComponent("p411").getProperty(
        "p411.symbolic_name"
    ).setValue("PANEL_RESET" if owner else "")

    problems = list(configuration.getProblems())
    for problem in problems:
        emit("CHILD_PROBLEM=" + str(problem))
    require(not problems, "Child FSP configuration validation failed")
    configuration.save()


def solution_private_field(solution_configuration, field_name):
    field = solution_configuration.getClass().getDeclaredField(field_name)
    field.setAccessible(True)
    return field.get(solution_configuration)


def configure_solution(solution_project):
    adapters = jvm.org.eclipse.core.runtime.Adapters
    project_class = jvm.java.lang.Class.forName(
        "com.renesas.cdt.ddsc.project.IDDSCProject"
    )
    ddsc_project = adapters.adapt(solution_project, project_class)
    require(ddsc_project is not None, "Solution is not an IDDSCProject")

    solution_type = (
        jvm.com.renesas.cdt.ddsc.scripting.configuration.DDSCSolutionConfiguration
    )
    solution_configuration = solution_type(ddsc_project, "6.4.0")
    document = solution_private_field(solution_configuration, "fSolutionConfigDoc")
    settings = solution_private_field(solution_configuration, "fConfigSettings")
    target = str(settings.getPropertyValue("#TargetName#"))
    version = str(settings.getPropertyValue("#FSPVersion#"))
    require(target == "R7KA8P1KFLCAC", "Unexpected Solution target: " + target)
    require(version == "6.4.0", "Unexpected Solution FSP version: " + version)

    pin_factory = jvm.com.renesas.cdt.ddsc.internal.contentgen.pins.PinConfigFactory
    config_node = pin_factory.createLoader(
        document, ddsc_project.getLocation()
    ).getConfigNode()
    require(config_node is not None, "Solution raPinConfiguration node is missing")

    mapping_locator = (
        jvm.com.renesas.cdt.ddsc.internal.contentgen.pins.PinMappingLocator
    )
    mapping_file = mapping_locator(
        ddsc_project.getDeviceFamilyContext()
    ).getMappingFile(target, version)
    require(mapping_file is not None and mapping_file.isFile(), "Pin mapping is missing")

    facade = jvm.com.renesas.tools.pinconfigurator.api.PinConfiguratorFacade
    key = solution_project.getName()
    built = (
        facade.buildPinConfigurator()
        .mapTo(key)
        .parsing(mapping_file)
        .loadConfiguration(config_node)
        .setWorkingDirectory(ddsc_project.getLocation().toFile().toPath())
        .build()
    )
    require(built, "Solution PinConfigurator model did not build")

    try:
        pin_configurator = jvm.com.renesas.tools.pinconfigurator.PinConfigurator
        pin_set = (
            pin_configurator.getInstance(key)
            .getModelWrapper()
            .getPinConfigSet()
        )
        target_model = pin_set.getPinConfig("RA8P1_CPKHMI.pincfg")
        require(target_model is not None, "RA8P1 Solution pin profile is missing")
        pin_set.activePinConfig(target_model)
        pin_set.selectPinConfig(target_model)
        target_model.setSymbol("g_bsp_pin_cfg")
        for pin_model in pin_set.getPinConfigs():
            if (
                str(pin_model.getName()) != "RA8P1_CPKHMI.pincfg"
                and str(pin_model.getSymbol()) == "g_bsp_pin_cfg"
            ):
                pin_model.setSymbol("")

        scripting = facade.getPinScriptingProvider(key)
        target_configuration = pin_configuration_by_name(
            scripting.getPinConfigurations(), "RA8P1_CPKHMI.pincfg"
        )
        p411 = target_configuration.getComponent("p411")
        require(p411 is not None, "Solution P411 component is missing")
        p411.getProperty("p411.symbolic_name").setValue("PANEL_RESET")
        p411.getConfig("p411.gpio_mode").setValue(
            "p411.gpio_mode.gpio_mode_out.low"
        )

        pin_factory.createSaver().save(key, document)
        solution_configuration.save()
    finally:
        facade.removeInstance(key)

    emit("SOLUTION_TARGET=" + target)
    emit("SOLUTION_FSP=" + version)
    emit("SOLUTION_PROFILE=RA8P1_CPKHMI.pincfg")
    emit("SOLUTION_P411=PANEL_RESET|gpio_output_low")


try:
    loadModule("/FSP/Configuration")
    loadModule("/FSP/Resources")

    solution_project = importProject(argv[2])
    cpu0_project = importProject(argv[3])
    cpu1_project = importProject(argv[4])

    if solution_is_configured(argv[2]):
        emit("SOLUTION_ALREADY_CONFIGURED=1")
    else:
        configure_solution(solution_project)

    cpu0_configuration = openDDSCConfigurationWithVersion(
        cpu0_project.getName(), "6.4.0"
    )
    if child_is_configured(argv[3], "RA8P1_Competition_Board", False):
        emit("CPU0_ALREADY_CONFIGURED=1")
    else:
        configure_child(
            cpu0_configuration,
            cpu0_project.getName(),
            "RA8P1_Competition_Board",
            False,
        )
    emit("CPU0_P411=unassigned")

    cpu1_configuration = openDDSCConfigurationWithVersion(
        cpu1_project.getName(), "6.4.0"
    )
    if child_is_configured(argv[4], "RA8P1_CPKHMI.pincfg", True):
        emit("CPU1_ALREADY_CONFIGURED=1")
    else:
        configure_child(
            cpu1_configuration,
            cpu1_project.getName(),
            "RA8P1_CPKHMI.pincfg",
            True,
        )
    emit("CPU1_PROFILE=RA8P1_CPKHMI.pincfg")
    emit("CPU1_P411=PANEL_RESET|gpio_output_low")
    emit("P411_FSP_MUTATION_OK")
finally:
    output.close()
