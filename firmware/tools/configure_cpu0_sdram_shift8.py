loadModule("/FSP/Configuration")
loadModule("/FSP/Resources")

ResourcesPlugin = jvm.org.eclipse.core.resources.ResourcesPlugin
project_name = jvm.java.io.File(argv[1]).getName()
project = ResourcesPlugin.getWorkspace().getRoot().getProject(project_name)
if not project.exists():
    project = importProject(argv[1])

configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")
property_id = "config.bsp.fsp.sdram.addr_shift"
value_id = "config.bsp.fsp.sdram.addr_shift.8"
prop = configuration.getBSP().getProperty(property_id)
if prop is None:
    raise RuntimeError("CPU0 SDRAM address-shift property is unavailable")

options = [str(option) for option in prop.getOptions()]
if value_id not in options:
    raise RuntimeError("CPU0 SDRAM shift 8 is unavailable: " + str(options))

prop.setValue(value_id)
if str(prop.getValue()) != value_id:
    raise RuntimeError("CPU0 SDRAM shift update failed")

problems = list(configuration.getProblems())
if problems:
    for problem in problems:
        print("PROBLEM=" + str(problem))
    raise RuntimeError("Configuration validation failed")

configuration.save()
configuration.generateProjectContent(None)
print("CPU0_SDRAM_SHIFT=" + property_id + "|" + str(prop.getValue()))
