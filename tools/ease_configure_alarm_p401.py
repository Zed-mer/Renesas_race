# EASE Python mutation for the J201-3 (P401) active-low alarm buzzer.
loadModule("/FSP/Configuration")
loadModule("/FSP/Resources")

project = importProject(argv[1])
configuration = openDDSCConfigurationWithVersion(project.getName(), "6.4.0")

PinConfigurator = jvm.com.renesas.tools.pinconfigurator.PinConfigurator
pin_model = PinConfigurator.getInstance(project.getName()).getModelWrapper().getPinConfigSet()
target_model = pin_model.getPinConfig("RA8P1_CPKHMI.pincfg")
pin_model.activePinConfig(target_model)
pin_model.selectPinConfig(target_model)
target_model.setSymbol("g_bsp_pin_cfg")

pin_cfg = configuration.getPinConfiguration("RA8P1_CPKHMI.pincfg")
buzzer = pin_cfg.getComponent("p401")
mode = buzzer.getConfig("p401.gpio_mode")
options = list(mode.getOptions())
selected = "p401.gpio_mode.gpio_mode_out.high"
print("P401_GPIO_OPTIONS=" + str(options))
if selected not in options:
    raise RuntimeError("P401 output-high option is unavailable: " + str(options))
mode.setValue(selected)
buzzer.getProperty("p401.symbolic_name").setValue("ALARM_BUZZER")

problems = list(configuration.getProblems())
for problem in problems:
    print("PROBLEM=" + str(problem))
if problems:
    raise RuntimeError("Configuration validation failed before save")

configuration.save()
configuration.generateProjectContent(None)
print("RA8P1_ALARM_P401_MUTATION_OK")
