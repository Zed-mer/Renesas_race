loadModule("/FSP/Configuration")
loadModule("/FSP/Resources")

ResourcesPlugin = jvm.org.eclipse.core.resources.ResourcesPlugin
project_name = jvm.java.io.File(argv[1]).getName()
project = ResourcesPlugin.getWorkspace().getRoot().getProject(project_name)
if not project.exists():
    project = importProject(argv[1])

configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")

field = configuration.getClass().getSuperclass().getDeclaredField("fComponents")
field.setAccessible(True)
components = field.get(configuration)
key = "LVGL##GUI##all##lvgl####9.3.0+renesas.0.fsp.6.4.0"
component = components.findModule(key)
if component is None or not component.getParentPackAvailable():
    raise RuntimeError("LVGL component unavailable: " + key)

components.setSelected(key, True)
if not component.getSelected():
    raise RuntimeError("LVGL component selection failed")

problems = list(configuration.getProblems())
if problems:
    for problem in problems:
        print("PROBLEM=" + str(problem))
    raise RuntimeError("Configuration validation failed")

configuration.save()
configuration.generateProjectContent(None)
print("LVGL_COMPONENT_WRITE_OK=" + key)
