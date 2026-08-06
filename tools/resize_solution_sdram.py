loadModule('/FSP/Configuration')
loadModule('/FSP/Resources')

ResourcesPlugin = jvm.org.eclipse.core.resources.ResourcesPlugin
project_name = jvm.java.io.File(argv[1]).getName()
project = ResourcesPlugin.getWorkspace().getRoot().getProject(project_name)
if not project.exists():
    project = importProject(argv[1])

Adapters = jvm.org.eclipse.core.runtime.Adapters
IDDSCProject = jvm.java.lang.Class.forName('com.renesas.cdt.ddsc.project.IDDSCProject')
SolutionConfiguration = jvm.com.renesas.cdt.ddsc.scripting.configuration.DDSCSolutionConfiguration
ddsc_project = Adapters.adapt(project, IDDSCProject)
if ddsc_project is None:
    raise RuntimeError('Project is not an FSP Solution Project')

configuration = SolutionConfiguration(ddsc_project, '6.4.0')
partitions = list(configuration.getMemoryPartitions())
cpu0 = next((p for p in partitions if str(p.getName()) == 'SDRAM_CPU0_S'), None)
cpu1 = next((p for p in partitions if str(p.getName()) == 'SDRAM_CPU1_S'), None)
if cpu0 is None or cpu1 is None:
    raise RuntimeError('SDRAM CPU partitions are missing')

cpu0.setOffset(0x0000000)
cpu0.setSize(0x0B00000)
cpu1.setOffset(0x0B00000)
cpu1.setSize(0x0500000)
configuration.save()

BundleGenerator = jvm.com.renesas.cdt.ddsc.internal.contentgen.builder.DdscSolutionBundleGenerator
output_folder = project.getFolder('build')
if not output_folder.exists():
    output_folder.create(True, True, None)
solution_path = project.getFile('solution.xml').getLocation()
bundle_path = output_folder.getLocation().append(project.getName()).addFileExtension('sbd')
BundleGenerator().generateBundle(ddsc_project.getDeviceFamilyContext(), solution_path, bundle_path)
output_folder.refreshLocal(1, None)
print('SOLUTION_SDRAM=CPU0:0x68000000+0x0B00000|CPU1:0x68B00000+0x0500000')
