loadModule("/FSP/Configuration")
loadModule("/FSP/Resources")

project = importProject(argv[1])
configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")

target = None
for thread in configuration.getThreads():
    for stack in thread.getStacks():
        if "ether_on_rmac" in stack.getId():
            target = stack

if target is None:
    raise RuntimeError("RMAC Ethernet stack not found")

print("RMAC_STACK=" + target.getId())
for property_id in [
    "module.driver.ether.num_tx_descriptors",
    "module.driver.ether.num_rx_descriptors",
    "module.driver.ether.node_num",
    "module.driver.ether.ether_buffer_size",
    "module.driver.rmac.rx_queue_length",
]:
    prop = target.getProperty(property_id)
    if prop is None:
        raise RuntimeError("RMAC property not found: " + property_id)
    print(
        "RMAC_PROP="
        + property_id
        + "|"
        + str(prop.getValue())
        + "|"
        + str(list(prop.getOptions()))
    )

problems = list(configuration.getProblems())
print("RMAC_PROBLEMS=" + str(len(problems)))
for problem in problems:
    print("RMAC_PROBLEM=" + str(problem))
