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

rx_descriptors = int(argv[2])
tx_descriptors = int(target.getProperty(
    "module.driver.ether.num_tx_descriptors"
).getValue())
node_count = tx_descriptors + rx_descriptors + 1

target.getProperty("module.driver.ether.num_rx_descriptors").setValue(
    str(rx_descriptors)
)
target.getProperty("module.driver.ether.node_num").setValue(str(node_count))

problems = list(configuration.getProblems())
if problems:
    raise RuntimeError("; ".join([str(problem) for problem in problems]))

configuration.save()
configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")

target = None
for thread in configuration.getThreads():
    for stack in thread.getStacks():
        if "ether_on_rmac" in stack.getId():
            target = stack
if target is None:
    raise RuntimeError("RMAC Ethernet stack missing after reload")
if target.getProperty("module.driver.ether.num_rx_descriptors").getValue() != str(rx_descriptors):
    raise RuntimeError("RMAC RX descriptor value did not persist")
if target.getProperty("module.driver.ether.node_num").getValue() != str(node_count):
    raise RuntimeError("RMAC node count did not persist")
problems = list(configuration.getProblems())
if problems:
    raise RuntimeError("; ".join([str(problem) for problem in problems]))

configuration.generateProjectContent(None)
print(
    "RMAC_DESCRIPTOR_WRITE_OK="
    + str(rx_descriptors)
    + "|"
    + str(node_count)
)
