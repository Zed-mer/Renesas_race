loadModule("/FSP/Configuration")
loadModule("/FSP/Resources")


EXPECTED_VALUES = {
    "config.bsp.option_setting.ofs0": "config.bsp.option_setting.ofs0.enabled",
    "config.bsp.option_setting.ofs2": "config.bsp.option_setting.ofs2.enabled",
    "config.bsp.option_setting.ofs2.cvm_reset": "config.bsp.option_setting.ofs2.cvm_reset.disabled",
    "config.bsp.option_setting.ofs2.npusa": "config.bsp.option_setting.ofs2.npusa.secure",
    "config.bsp.option_setting.ofs1_sec": "config.bsp.option_setting.ofs1_sec.enabled",
    "config.bsp.option_setting.ofs1_sel": "config.bsp.option_setting.ofs1_sel.enabled",
    "config.bsp.option_setting.ofs3_sec": "config.bsp.option_setting.ofs3_sec.enabled",
    "config.bsp.option_setting.ofs3_sel": "config.bsp.option_setting.ofs3_sel.enabled",
}


project = importProject(argv[1])
configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")
bsp = configuration.getBSP()

for property_id, expected_value in EXPECTED_VALUES.items():
    prop = bsp.getProperty(property_id)
    options = list(prop.getOptions())
    if expected_value not in options:
        raise RuntimeError(
            "Unsupported FSP value: " + property_id + "=" + expected_value +
            " options=" + str(options)
        )
    prop.setValue(expected_value)
    print("OFS_SET=" + property_id + "|" + prop.getValue())

problems = list(configuration.getProblems())
if problems:
    for problem in problems:
        print("PROBLEM=" + str(problem))
    raise RuntimeError("Configuration validation failed")

configuration.save()
configuration.generateProjectContent(None)

for property_id, expected_value in EXPECTED_VALUES.items():
    actual_value = bsp.getProperty(property_id).getValue()
    if actual_value != expected_value:
        raise RuntimeError(
            "OFS verification failed: " + property_id +
            " expected=" + expected_value + " actual=" + actual_value
        )

print("OFS_RECOVERY_CONFIGURATION_OK")
