loadModule("/FSP/Configuration")
loadModule("/FSP/Resources")

project = importProject(argv[1])
configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")


def find_rmac_stack(ddsc_configuration):
    for thread in ddsc_configuration.getThreads():
        for stack in thread.getStacks():
            if "ether_on_rmac" in stack.getId():
                return stack
    raise RuntimeError("RMAC Ethernet stack not found")


target = find_rmac_stack(configuration)
queue_length = int(argv[2])
expected_current = int(argv[3]) if len(argv) > 3 else None

queue_property = target.getProperty("module.driver.rmac.rx_queue_length")
queue_count_property = target.getProperty("module.driver.rmac.rx_queue_num")
descriptor_property = target.getProperty("module.driver.ether.num_rx_descriptors")
if queue_property is None or queue_count_property is None or descriptor_property is None:
    raise RuntimeError("Required RMAC RX queue properties were not found")

current_length = int(queue_property.getValue())
queue_count = int(queue_count_property.getValue())
descriptor_count = int(descriptor_property.getValue())
required_descriptors = queue_count * (queue_length + 1)

if expected_current is not None and current_length != expected_current:
    raise RuntimeError(
        "RMAC RX queue length changed unexpectedly: expected "
        + str(expected_current)
        + ", found "
        + str(current_length)
    )
if queue_length < 1:
    raise RuntimeError("RMAC RX queue length must be positive")
if required_descriptors > descriptor_count:
    raise RuntimeError(
        "RMAC RX queues require "
        + str(required_descriptors)
        + " descriptors, but only "
        + str(descriptor_count)
        + " RX descriptors are configured"
    )

queue_property.setValue(str(queue_length))
problems = list(configuration.getProblems())
if problems:
    raise RuntimeError("; ".join([str(problem) for problem in problems]))

configuration.save()
configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")
target = find_rmac_stack(configuration)

persisted_length = int(
    target.getProperty("module.driver.rmac.rx_queue_length").getValue()
)
persisted_queue_count = int(
    target.getProperty("module.driver.rmac.rx_queue_num").getValue()
)
persisted_descriptor_count = int(
    target.getProperty("module.driver.ether.num_rx_descriptors").getValue()
)
if persisted_length != queue_length:
    raise RuntimeError("RMAC RX queue length did not persist")
if persisted_queue_count != queue_count:
    raise RuntimeError("RMAC RX queue count changed unexpectedly")
if persisted_descriptor_count != descriptor_count:
    raise RuntimeError("RMAC RX descriptor count changed unexpectedly")

problems = list(configuration.getProblems())
if problems:
    raise RuntimeError("; ".join([str(problem) for problem in problems]))

configuration.generateProjectContent(None)
print(
    "RMAC_RX_QUEUE_WRITE_OK="
    + str(current_length)
    + "->"
    + str(queue_length)
    + "|queues="
    + str(queue_count)
    + "|descriptors="
    + str(descriptor_count)
    + "|active="
    + str(required_descriptors)
)
