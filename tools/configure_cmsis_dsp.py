loadModule("/FSP/Configuration")
loadModule("/FSP/Resources")

project = importProject(argv[1])
configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")

field = configuration.getClass().getSuperclass().getDeclaredField("fComponents")
field.setAccessible(True)
components = field.get(configuration)
key = "Arm##CMSIS##Main##DSP####1.16.2+fsp.6.4.0"
component = components.findModule(key)
if component is None or not component.getParentPackAvailable():
    raise RuntimeError("CMSIS-DSP component unavailable: " + key)

components.setSelected(key, True)
if not component.getSelected():
    raise RuntimeError("CMSIS-DSP selection failed")

problems = list(configuration.getProblems())
if problems:
    raise RuntimeError("; ".join([str(p) for p in problems]))

configuration.save()
configuration.generateProjectContent(None)
print("CMSIS_DSP_WRITE_OK=" + key)
