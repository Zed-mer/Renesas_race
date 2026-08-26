loadModule("/FSP/Configuration")
loadModule("/FSP/Resources")

project = importProject(argv[1])
mode = argv[2] if len(argv) > 2 else "probe"
configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")


def walk_stacks(stack):
    yield stack
    for interface in stack.getInterfaces():
        if interface.hasModule():
            for descendant in walk_stacks(interface.getModule()):
                yield descendant


targets = []
seen = set()
for thread in configuration.getThreads():
    for root in thread.getStacks():
        for stack in walk_stacks(root):
            stack_id = str(stack.getId())
            if stack_id in seen:
                continue
            seen.add(stack_id)

            properties = {}
            for prop in stack.getProperties():
                properties[str(prop.getId())] = prop

            if stack_id.startswith("module.driver.ether_on_rmac."):
                name = properties.get("module.driver.ether.name")
                if name is not None and str(name.getValue()) == "g_ether0":
                    targets.append((stack_id, properties["module.driver.ether.flow_control"]))

            if stack_id.startswith("module.driver.ether_phy_on_rmac_phy."):
                name = properties.get("module.driver.ether_phy.name")
                if name is not None and str(name.getValue()) == "g_rmac_phy0":
                    targets.append((stack_id, properties["module.driver.ether_phy.flow_control"]))

if len(targets) != 2:
    raise RuntimeError("Expected g_ether0 and g_rmac_phy0 flow-control properties, found " + str(len(targets)))

for stack_id, prop in targets:
    options = [str(option) for option in prop.getOptions()]
    print("FLOW_PROPERTY=" + stack_id + "|" + str(prop.getValue()) + "|" + str(options))
    if mode == "enable":
        enabled_value = str(prop.getId()) + ".1"
        if enabled_value not in options:
            raise RuntimeError("Enable option not available for " + str(prop.getId()))
        prop.setValue(enabled_value)

if mode == "enable":
    problems = list(configuration.getProblems())
    if problems:
        raise RuntimeError("; ".join([str(problem) for problem in problems]))
    configuration.save()
    configuration.generateProjectContent(None)
    print("FLOW_CONTROL_WRITE_OK=2")
elif mode != "probe":
    raise RuntimeError("Mode must be probe or enable")
