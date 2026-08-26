loadModule("/FSP/Configuration")
loadModule("/FSP/Resources")

import os


def open_project(project_path):
    target_path = os.path.normcase(os.path.abspath(project_path))
    ResourcesPlugin = jvm.org.eclipse.core.resources.ResourcesPlugin
    workspace_root = ResourcesPlugin.getWorkspace().getRoot()
    for candidate in workspace_root.getProjects():
        location = candidate.getLocation()
        if location is None:
            continue
        candidate_path = os.path.normcase(os.path.abspath(location.toOSString()))
        if candidate_path == target_path:
            return candidate
    return importProject(project_path)


def find_stack(configuration, id_fragment):
    for thread in configuration.getThreads():
        for stack in thread.getStacks():
            if id_fragment in stack.getId():
                return stack
    raise RuntimeError("Stack not found: " + id_fragment)


def require_clock(configuration, node_id, expected):
    node = configuration.getClockNode(node_id)
    options = [str(option) for option in node.getOptions()]
    print("CLOCK_OPTIONS=" + node_id + "|" + ",".join(options))
    actual = str(node.getValue())
    if actual != expected or expected not in options:
        raise RuntimeError("Unexpected clock option: " + node_id + "=" + actual)


def set_property(stack, property_id, value):
    prop = stack.getProperty(property_id)
    options = [str(option) for option in prop.getOptions()]
    print("PROPERTY_OPTIONS=" + property_id + "|" + ",".join(options))
    prop.setValue(value)
    if str(prop.getValue()) != value:
        raise RuntimeError("Property did not apply: " + property_id)


def property_int(stack, property_id):
    return int(str(stack.getProperty(property_id).getValue()))


def validate_contract(configuration):
    display = find_stack(configuration, "display_on_glcdc")
    require_clock(configuration, "board.clock.pll2.source",
                  "board.clock.pll2.source.xtal")
    require_clock(configuration, "board.clock.pll2.div",
                  "board.clock.pll2.div.3")
    require_clock(configuration, "board.clock.pll2.mul",
                  "board.clock.pll2.mul.300_00")
    require_clock(configuration, "board.clock.pll2r.div",
                  "board.clock.pll2r.div.5")
    require_clock(configuration, "board.clock.lcdclk.source",
                  "board.clock.lcdclk.source.pll2r")
    require_clock(configuration, "board.clock.lcdclk.div",
                  "board.clock.lcdclk.div.2")

    divisor = str(display.getProperty(
        "module.driver.display.clock_div_ratio").getValue())
    expected_divisor = (
        "module.driver.display.clock_div_ratio.panel_clk_divisor_6"
    )
    if divisor != expected_divisor:
        raise RuntimeError("Unexpected GLCDC divisor: " + divisor)

    htotal = property_int(
        display, "module.driver.display.output.htiming.total_cyc")
    hactive = property_int(
        display, "module.driver.display.output.htiming.display_cyc")
    hback_start = property_int(
        display, "module.driver.display.output.htiming.back_porch")
    hsync = property_int(
        display, "module.driver.display.output.htiming.sync_width")
    vtotal = property_int(
        display, "module.driver.display.output.vtiming.total_cyc")
    vactive = property_int(
        display, "module.driver.display.output.vtiming.display_cyc")
    vback_start = property_int(
        display, "module.driver.display.output.vtiming.back_porch")
    vsync = property_int(
        display, "module.driver.display.output.vtiming.sync_width")

    if (htotal, hactive, hback_start, hsync) != (1344, 1024, 136, 24):
        raise RuntimeError("Horizontal timing contract mismatch")
    if (vtotal, vactive, vback_start, vsync) != (635, 600, 21, 2):
        raise RuntimeError("Vertical timing contract mismatch")
    if (hback_start - hsync,
            htotal - hactive - hback_start - hsync) != (112, 160):
        raise RuntimeError("Generated horizontal DSI porch mismatch")
    if (vback_start - vsync,
            vtotal - vactive - vback_start - vsync) != (19, 12):
        raise RuntimeError("Generated vertical DSI porch mismatch")
    return display


project = open_project(argv[1])
configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")
print("SUMMARY=" + str(configuration.getSummary()))

display = find_stack(configuration, "display_on_glcdc")
set_property(display, "module.driver.display.output.htiming.total_cyc", "1344")
set_property(display, "module.driver.display.output.htiming.back_porch", "136")
set_property(display, "module.driver.display.output.vtiming.back_porch", "21")
validate_contract(configuration)

problems = list(configuration.getProblems())
if problems:
    raise RuntimeError("; ".join([str(problem) for problem in problems]))
configuration.save()

reloaded = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")
validate_contract(reloaded)
reload_problems = list(reloaded.getProblems())
if reload_problems:
    raise RuntimeError("; ".join([str(problem) for problem in reload_problems]))

print("DISPLAY_V27_STABLE_CONFIG_OK")
print("CONTENT_GENERATION_REQUIRED=solution-build")
print("PIXEL_CLOCK_HZ=40000000")
print("HTOTAL=1344")
print("HBP=112")
print("HFP=160")
print("VTOTAL=635")
print("VBP=19")
print("VFP=12")
print("EXPECTED_DSI_DELAY=184")
print("REFRESH_MILLIHZ=46869")
